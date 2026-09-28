# 交接文档：给 PS2SDK 打补丁，修复 coverflow/浏览偶发卡死（fd/handle 耗尽 -EMFILE）

> 本文件是给**新会话的 agent** 的完整交接。目标：把下方补丁提交到用户的 PS2SDK fork 并开 PR。
> 目标仓库：`https://github.com/362053534/ps2sdk`（fork 自 `ps2dev/ps2sdk`，默认分支 `master`）。
>
> **本次共两条链路、三份补丁，分属两个仓库：**
>
> | # | 仓库 | 文件 / 函数 | 说明 | 详见 |
> |---|------|-------------|------|------|
> | 1 | `362053534/ps2sdk` | `iop/network/smbman/src/smb_fio.c` → `smb_close()` | **SMB 链路根修（必做）** | 第 2~6 节 |
> | 2 | **`362053534/FatFs-PS2OPL`（分支 `iop-r0.15`）** | `source/ff.c` → `f_close()` / `f_closedir()` | **BDM 链路根修（推荐主修）** | 第 9 节 |
> | 3 | `362053534/ps2sdk` | `iop/fs/bdmfs_fatfs/src/fs_driver.c` → `fs_close()` / `fs_dclose()` | BDM 链路驱动侧防御纵深（可选，与 #2 二选一或都要） | 第 9 节 |
>
> 三者是**同一类 bug**：close 时遇到网络/设备抖动，句柄不被回收，本地句柄槽位逐渐漏满
> （smbman 32、bdmfs 文件 128 / 目录 16）后所有 `open()`/`opendir()` 返回 `-EMFILE`，
> 表现为浏览一段时间后设备/游戏列表清空、冻死。coverflow 因 art open/close 量大而更易触发。
>
> **BDM 修在哪一层？** 缺陷本体在 FatFs 的 `f_close`/`f_closedir`（出错路径不清 `obj.fs`），
> 所以 **#2（FatFs-PS2OPL）才是根上修**，一处覆盖所有用这套 FatFs 的驱动（当前仓内只有
> `bdmfs_fatfs`；`bdmfs_vfat` 不用它、HDD 走 PFS）。**#3 是驱动侧防御**（驱动强制回收自己
> 的槽位），更保险且让驱动自洽，但非必需。推荐至少做 #1 + #2；#3 视口味决定。
>
> **APA / HDD（PFS）链路已核查：不受影响，无需改动。** 详见第 11 节。

---

## 0. 你要做的事（TL;DR）

1. clone `362053534/ps2sdk`，建分支 `fix/smbman-fdhandle-leak`。
2. 按第 3 节把 `smb_close()` 替换成"修复后"的版本（或直接 `git am` 第 6 节的补丁文件）。
3. commit（提交说明用中文，见第 5 节）、push、`gh pr create` 到 `362053534/ps2sdk:master`。
4. 把 PR 链接回报给用户。
5. 用户会重编 `smbman.irx` 装进 `$(PS2SDK)/iop/irx/`，再重编 OPL 验证（见第 7 节）。

> 权限提醒：给 SDK 打补丁需要当前会话的 GitHub 令牌能访问 `362053534/ps2sdk`。
> 先自检：`gh api installation/repositories --jq '.repositories[].full_name'`，确认列表里有 `362053534/ps2sdk`。
> 若只看到 `362053534/Open-PS2-Loader`，说明令牌还没覆盖该仓库，push 会 403 —— 让用户在 Arena 里重连 GitHub 后重开会话再试。

---

## 1. 背景与问题现象

- 项目：Open-PS2-Loader（OPL），PS2 上的游戏加载器；用户在做 **coverflow**（封面流）第二套内置主题。
- 症状：用 **coverflow 通过 SMB（网络/eth 模式）浏览游戏**时，**偶发彻底卡死**——浏览一段时间后游戏列表突然清空、怎么刷新都回不来。普通主题几乎不触发，coverflow 明显更容易触发。

## 2. 根因（已用实机日志 + 源码审计确认）

实机日志呈现清晰的两阶段：
- **正常期**：大量封面/背景/截图 art 通过 SMB **加载成功**；偶发个别游戏 art `open FAILED ... errno=5`（EIO，就是没那张图，正常）。
- **崩点**：**几乎所有 open() 突然变 `errno=24`**，包括 `texLoadAll open FAILED errno=24`、成片 `jpgOpen: error opening`、最终 `sbReadList: opendir CD/DVD BOTH failed errnoCD=24 errnoDVD=24`、`APPS failed to open dir smb:APPS`。之后一切 open/opendir 全失败 → 列表清空 → 冻死。

**`errno=24` = `-EMFILE` = 打开文件数超限。** 已排查确认 OPL 自身 EE 侧所有 open/close 都配对（`config.c`、`util.c` 的 openFileBuffer/closeFileBuffer/readFile、`textures.c:texLoadAll` 的 PNG 路径、外部 `libjpg_ps2_addons` 的 `jpgFromFilename` —— 全部干净）。日志里的 `jpgOpen: error opening` 出现在 errno=24 **之后**，是耗尽的症状不是原因。

真正的天花板与泄漏在 **PS2SDK 的 smbman 驱动**（OPL 浏览用的 `smb:` 设备，来自 `$(PS2SDK)/iop/irx/smbman.irx`，OPL 与上游都直接用预编译版、不带源码）：

- `iop/network/smbman/src/smb_fio.c`：`#define MAX_FDHANDLES 32` —— `smb:` 设备**总共只有 32 个文件/目录 handle 槽位**；满了 `smb_open`/`smb_dopen` 返回 `-EMFILE`。
- **`smb_close()` 有两处会"跳过释放本地槽位"**，把 32 个槽位慢慢漏光：
  1. **网络关闭失败漏**：`r = smb_Close(...); if (r != 0) goto io_unlock;` —— 远端 CIFS Close 请求一旦因网络抖动失败，就跳过后面的 `memset(fh,0,...)`，槽位永久占用。浏览时偶发的 EIO 抖动每命中一次 close 就漏一个。
  2. **只探测不读目录漏 / 会话临时掉线漏**：函数开头 `if ((UID==-1)||(TID==-1)||(fh->smb_fid==-1)) return -EBADF;` 会**提前返回、不释放槽位**。而 `smb_dopen` 只占槽位、不设 `smb_fid`（保持 -1，只有第一次 `smb_dread` 才设）——所以任何 **`opendir` 后不 `readdir` 就 `closedir`** 的"存在性探测"都会漏。OPL 里 `sbArtDirExists`、`sbReadList` 的 CD/DVD 探测、`sbDetectArtBuckets` 都是这种模式。

两条都在耗同一个 32 槽表 → 漏满 → `-EMFILE` → 完美复现日志两阶段。coverflow 每次导航的 open/close 量是普通主题的 2–3 倍，所以只有它容易在几十秒内漏满。

## 3. 修复方案（改 `smb_close`）

核心原则：**无论远端 Close 成败、无论会话是否在线，都始终回收本地 handle 槽位。** 本地句柄是本地资源，远端 Close 失败时服务器端会在会话超时/断开时自行清理；漏掉本地槽位才是把客户端拖死的元凶。

### 修复前（buggy，当前 `362053534/ps2sdk` = 上游原样）
```c
int smb_close(iop_file_t *f)
{
    FHANDLE *fh = (FHANDLE *)f->privdata;
    int r       = 0;

    if ((UID == -1) || (TID == -1) || (fh->smb_fid == -1))
        return -EBADF;

    smb_io_lock();

    if (fh) {
        if (fh->mode != O_DIROPEN) {
            r = smb_Close(UID, TID, fh->smb_fid);
            if (r != 0) {
                goto io_unlock;
            }
        }
        memset(fh, 0, sizeof(FHANDLE));
        fh->smb_fid = -1;
        r           = 0;
    }

io_unlock:
    smb_io_unlock();

    return r;
}
```

### 修复后（目标）
```c
int smb_close(iop_file_t *f)
{
    FHANDLE *fh = (FHANDLE *)f->privdata;
    int r       = 0;

    if (fh == NULL)
        return -EBADF;

    smb_io_lock();

    // Only send the remote Close request when we actually have a live session
    // and a real (non-directory) server file id. Directory handles that were
    // opened but never read still have smb_fid == -1 and must not be sent.
    if (fh->mode != O_DIROPEN && fh->smb_fid != -1 && UID != -1 && TID != -1)
        r = smb_Close(UID, TID, fh->smb_fid);

    // Always reclaim the local file handle slot, regardless of whether the
    // remote Close succeeded or the session was dropped. Bailing out on error
    // (or on a temporarily disconnected session) used to leak the slot forever,
    // so a flaky network or a plain "opendir + closedir without readdir" would
    // slowly exhaust all MAX_FDHANDLES entries and every later open()/opendir()
    // would then fail with -EMFILE.
    memset(fh, 0, sizeof(FHANDLE));
    fh->smb_fid = -1;

    smb_io_unlock();

    return r;
}
```

要点：
- 去掉开头的"提前 return -EBADF"分支，只保留 `fh==NULL` 空指针防护。
- 去掉 `smb_Close` 失败后的 `goto io_unlock`（连同 `io_unlock:` 标号一并删除；该标号在本函数内已无其它引用，其它函数各自有自己的局部 `io_unlock:`，不受影响）。
- 只有"会话在线 + 非目录 + smb_fid 有效"时才发远端 Close，之后**始终** `memset` 回收槽位。
- 仍返回 `r`（远端 Close 的结果）——调用方能拿到错误码，但本地 fd 已释放，符合 POSIX `close()` 语义。

## 4. 落地命令

```bash
# 1) clone + 建分支
git clone https://github.com/362053534/ps2sdk.git
cd ps2sdk
git checkout -b fix/smbman-fdhandle-leak

# 2) 打补丁：二选一
#   (a) 用现成补丁文件（见第 6 节，从 OPL 仓库根目录复制过来）
git am /path/to/smbman-fix-EMFILE-handle-leak.patch
#   (b) 或手动把 iop/network/smbman/src/smb_fio.c 的 smb_close 换成第 3 节"修复后"版本

# 3) 若手动改，则提交（提交说明见第 5 节）
git add iop/network/smbman/src/smb_fio.c
git commit -F commit-msg.txt   # 或 -m "..."（中文）

# 4) push + 开 PR
git push -u origin fix/smbman-fdhandle-leak
gh pr create --base master \
  --title "smbman: fix smb_close handle-slot leak causing -EMFILE" \
  --body "见提交说明；修复 smb_close 在远端 Close 失败/会话掉线/目录只探测不读时不回收本地 FHANDLE 槽位，导致 MAX_FDHANDLES(32) 逐渐漏满、后续 open/opendir 全部 -EMFILE。"
```

## 5. 提交说明（中文，用户要求 commit 用中文）

```
smbman: 修复 smb_close 泄漏文件句柄槽位导致 -EMFILE

smb_close 在两种情况下会跳过释放本地 FHANDLE 槽位：
1) 远端 smb_Close 返回非零（网络抖动）时 goto io_unlock，不 memset 槽位；
2) 会话临时掉线(UID/TID==-1) 或目录句柄只 opendir 未 readdir(smb_fid==-1)时
   在函数开头直接 return -EBADF。

两种情况都会永久占用槽位，网络不稳或频繁的"opendir+closedir 但不 readdir"
探测会把 MAX_FDHANDLES(32) 个槽位逐渐漏光，之后所有 open()/opendir() 都返回
-EMFILE。改为：仅在会话在线且有真实文件 id 时才发远端 Close，且无论结果如何
都始终回收本地槽位。
```

## 6. 现成补丁文件

OPL 仓库根目录下已有 `smbman-fix-EMFILE-handle-leak.patch`（`git format-patch` 格式，可直接 `git am`）。内容如下（若文件丢失，可据此重建，或直接用第 3 节手改）：

```diff
diff --git a/iop/network/smbman/src/smb_fio.c b/iop/network/smbman/src/smb_fio.c
index dce5b1a..175f7cb 100644
--- a/iop/network/smbman/src/smb_fio.c
+++ b/iop/network/smbman/src/smb_fio.c
@@ -373,24 +373,26 @@ int smb_close(iop_file_t *f)
     FHANDLE *fh = (FHANDLE *)f->privdata;
     int r       = 0;
 
-    if ((UID == -1) || (TID == -1) || (fh->smb_fid == -1))
+    if (fh == NULL)
         return -EBADF;
 
     smb_io_lock();
 
-    if (fh) {
-        if (fh->mode != O_DIROPEN) {
-            r = smb_Close(UID, TID, fh->smb_fid);
-            if (r != 0) {
-                goto io_unlock;
-            }
-        }
-        memset(fh, 0, sizeof(FHANDLE));
-        fh->smb_fid = -1;
-        r           = 0;
-    }
+    // Only send the remote Close request when we actually have a live session
+    // and a real (non-directory) server file id. Directory handles that were
+    // opened but never read still have smb_fid == -1 and must not be sent.
+    if (fh->mode != O_DIROPEN && fh->smb_fid != -1 && UID != -1 && TID != -1)
+        r = smb_Close(UID, TID, fh->smb_fid);
+
+    // Always reclaim the local file handle slot, regardless of whether the
+    // remote Close succeeded or the session was dropped. Bailing out on error
+    // (or on a temporarily disconnected session) used to leak the slot forever,
+    // so a flaky network or a plain "opendir + closedir without readdir" would
+    // slowly exhaust all MAX_FDHANDLES entries and every later open()/opendir()
+    // would then fail with -EMFILE.
+    memset(fh, 0, sizeof(FHANDLE));
+    fh->smb_fid = -1;
 
-io_unlock:
     smb_io_unlock();
 
     return r;
```

> 注：补丁 header 里的 blob hash（`dce5b1a..175f7cb`）和行号 `@@ -373,24 +373,26 @@` 基于抓取时的 `master`。若 `git am` 因上下文对不齐失败，改用 `git apply --3way` 或直接按第 3 节手改再提交（最稳妥）。

## 7. 验证（用户本地做，agent 无 EE/IOP 工具链无法编译）

1. 在 PS2SDK 里重编 smbman：`cd iop/network/smbman && make` → 得到新的 `smbman.irx`，装到 `$(PS2SDK)/iop/irx/smbman.irx`。
2. 重编 OPL（其 Makefile 用 `$(PS2SDK)/iop/irx/smbman.irx` 经 bin2c 内嵌）。
3. 实机：eth/SMB 模式 + coverflow 主题，长时间快速浏览大量游戏（尤其反复进出、翻很多有/无封面的游戏）。
4. 期望：不再出现列表突然清空/冻死；日志里不再出现成片 `errno=24`。
5. 可选回归：普通主题浏览、目录刷新、APPS 列表、写操作（配置/txt 重命名）均正常。

## 9. 追加：BDM 链路（bdmfs_fatfs）同类泄漏及修复

用户反馈走 **BDM（USB / BDMHDD / MX4SIO / SD 卡等块设备）也会遇到同样的 -EMFILE 卡死**。已确认 BDM 文件系统驱动有同类问题。

### 根因
- 文件：`iop/fs/bdmfs_fatfs/src/fs_driver.c`（对外是 `mass:` 设备；FatFs 库来自 `362053534/FatFs-PS2OPL@iop-r0.15`，位于 `common/external_deps/fatfs`，由 `download_dependencies.sh` 拉取）。
- 槽位：`#define MAX_FILES 128`（`fil_structures[]`）、`#define MAX_DIRS 16`（`dir_structures[]`）；"空闲"判定 = `obj.fs == NULL`；满了返回 `-EMFILE`。
- `fs_close`/`fs_dclose` 依赖 FatFs 的 `f_close`/`f_closedir` 去清 `obj.fs` 释放槽位。但 FatFs：
  - `f_close` 只有 `f_sync()` **且** `validate()` 都成功时才 `fp->obj.fs = 0`；块设备写回/重挂失败（USB 抖动、MX4SIO/SD 时序打嗝）→ 失败 → **不清槽位**。
  - `f_closedir` 只有 `validate()` 成功才 `dp->obj.fs = 0`；设备掉线/重枚举时失败 → **不清槽位**。目录槽仅 16 个，很快漏满。
- 累积到上限 → 后续 `open()`/`opendir()` 全 `-EMFILE`，与 smbman 同症。

### 修复位置：两层，推荐主修 #2（FatFs 根修）

BDM 有两个合理的修复点，二者都能解决问题：

- **#2 根修（推荐，提到 `362053534/FatFs-PS2OPL@iop-r0.15`）**：缺陷本体就在 FatFs，改这里一处治本，覆盖所有消费者。见 9A。
- **#3 驱动侧防御（可选，提到 `362053534/ps2sdk` 的 `fs_driver.c`）**：驱动层强制回收自己的槽位，不动第三方库。见 9B。

推荐至少做 #2；#3 作为防御纵深可留可去。

---

### 9A. #2 根修：`362053534/FatFs-PS2OPL@iop-r0.15` 的 `source/ff.c`

FatFs 的 `f_close` 只在 `f_sync()` **且** `validate()` 成功时清 `fp->obj.fs = 0`；`f_closedir` 只在 `validate()` 成功时清 `dp->obj.fs = 0`。上层驱动以 `obj.fs == NULL` 判定槽位空闲，所以出错不清 = 槽位泄漏。

改法：在两个函数 `return res;` 之前，**无论结果如何都把 `obj.fs` 清零**回收槽位。错误路径上不持有卷锁（validate 失败未加锁、f_sync 内部已 LEAVE 解锁），故安全；成功路径重复清零无害。FF_FS_LOCK=0 时不涉及 lockid 计数。

- 补丁文件（OPL 仓库根）：`FatFs-PS2OPL-fix-EMFILE-handle-leak.patch`
- 目标仓库/分支：`362053534/FatFs-PS2OPL`，基于 `iop-r0.15`；建议特性分支名 `fix/fclose-invalidate-on-error`
- 提交说明（中文）：
```
ff: f_close/f_closedir 出错时也失效对象，修复句柄槽位泄漏 -EMFILE

原实现仅在 f_sync 且 validate 成功(f_close)、或 validate 成功(f_closedir)时
才把 obj.fs 清零。块设备(USB/MX4SIO/SD)抖动、掉线或重枚举会让 flush/validate
失败，obj.fs 不被清空。上层 fs 驱动以 obj.fs==NULL 判定句柄槽位空闲，于是槽位
永久占用；累积到驱动的固定句柄池上限后，所有 open()/opendir() 返回 -EMFILE，
表现为浏览一段时间后设备列表清空/冻死。

PS2/OPL 场景介质会突然消失且驱动从不重试 close，故改为无论 f_sync/validate
结果如何都在返回前将 obj.fs 清零，回收槽位（错误路径上不持有卷锁，安全）。
```
- 修复后（`ff.c`，两个函数的尾部各加一段，示意 f_close）：
```c
        }
    }

    /* PS2/OPL fix: always invalidate the object so the fs-driver handle slot
       (detected via obj.fs == NULL) is reclaimed even when f_sync()/validate()
       failed on a removed or flaky medium. Without this, obj.fs stays set and
       the fixed handle pool leaks until every open()/opendir() returns -EMFILE. */
    fp->obj.fs = 0;   /* f_closedir 里对应写 dp->obj.fs = 0; */
    return res;
}
```
- 注意：`ff.c` 是 **CRLF + Tab** 文件，手改时保持行尾/缩进；直接 `git am` 补丁最稳。
- 落地：clone `362053534/FatFs-PS2OPL` 的 `iop-r0.15` → 建分支 → `git am FatFs-PS2OPL-fix-EMFILE-handle-leak.patch` → push → 对 `iop-r0.15` 开 PR。
  （OPL 构建时 `download_dependencies.sh` 会从该 fork 的 `iop-r0.15` 拉 FatFs，故 PR 合进 `iop-r0.15` 后重新拉依赖即可生效。）

---

### 9B. #3 驱动侧防御（可选）：改 `362053534/ps2sdk` 的 `iop/fs/bdmfs_fatfs/src/fs_driver.c`
在 `fs_close`/`fs_dclose` 里，无论 `f_close`/`f_closedir` 返回什么，都**在驱动层强制回收本地槽位**（把 `obj.fs` 清成 NULL —— 这正是 `fs_find_free_fil/dir_structure()` 判定空闲的字段）。即使只做 #2，这一层也无害；只做 #3 也能独立解决问题。

- 补丁文件（OPL 仓库根）：`bdmfs_fatfs-fix-EMFILE-handle-leak.patch`
- 建议分支名：`fix/bdmfs-fatfs-fdhandle-leak`
- 提交说明（中文）：
```
bdmfs_fatfs: 修复 fs_close/fs_dclose 泄漏句柄槽位导致 -EMFILE

fs_close/fs_dclose 依赖 FatFs 的 f_close/f_closedir 清 obj.fs 来释放本地槽位
（fs_find_free_fil/dir_structure 以 obj.fs==NULL 判定空闲）。但 FatFs 仅在
f_sync 且 validate 成功(f_close)、或 validate 成功(f_closedir)时才清 obj.fs。
块设备抖动/掉线/重枚举(USB、MX4SIO、SD)会让 flush/validate 失败，obj.fs 不被
清空，槽位永久占用；累积到 MAX_FILES(128)/MAX_DIRS(16) 即所有 open/opendir
返回 -EMFILE（尤其 16 个目录槽很快漏满）。

改为：无论 f_close/f_closedir 返回什么，都在驱动层强制 obj.fs=NULL 回收本地
槽位（不动第三方 FatFs 库，避免影响 HDD 等其它使用者）。
```

### 修复后代码（`fs_close`/`fs_dclose` 目标形态）
```c
static int fs_close(iop_file_t *fd)
{
    M_DEBUG("%s\n", __func__);

    int ret = FR_OK;

    _fs_lock();

    if (fd->privdata) {
        FIL *fil = (FIL *)fd->privdata;

        ret = f_close(fil);

        // Always release the local slot, even if f_close() failed (flush/validate
        // error on a flaky/removed block device); otherwise obj.fs stays set and
        // MAX_FILES slots slowly leak into -EMFILE.
        fil->obj.fs  = NULL;
        fd->privdata = NULL;
    }

    _fs_unlock();
    return -ret;
}

static int fs_dclose(iop_file_t *fd)
{
    M_DEBUG("%s\n", __func__);

    int ret = ENOENT;

    _fs_lock();

    if (fd->privdata) {
        DIR *dir = (DIR *)fd->privdata;

        ret = f_closedir(dir);

        // Always release the local slot, even if f_closedir() failed; otherwise
        // obj.fs stays set and the (only 16) MAX_DIRS slots quickly leak into -EMFILE.
        dir->obj.fs  = NULL;
        fd->privdata = NULL;
    }

    _fs_unlock();
    return -ret;
}
```

### BDM 验证
1. 若走 #2（FatFs 根修）：把修复合进 `362053534/FatFs-PS2OPL@iop-r0.15` 后，在 ps2sdk 里删掉已缓存的 `common/external_deps/fatfs` 并重新 `download_dependencies.sh` 拉取（或直接在缓存目录里 `git pull`），再 `cd iop/fs/bdmfs_fatfs && make` 得到新 `bdmfs_fatfs.irx` 装到 `$(PS2SDK)/iop/irx/`。
2. 若走 #3（驱动侧）：直接 `cd iop/fs/bdmfs_fatfs && make`（首次可能需先 `download_dependencies.sh` 拉 FatFs）。
3. 重编 OPL，用 USB / BDMHDD / MX4SIO / SD 设备 + coverflow 长时间浏览，尤其制造设备抖动/热插拔场景。
4. 期望：不再列表清空/冻死，日志无成片 `errno=24`。

---

## 10. 现状小结（交接时点）

- 三份补丁均已写好并本地验证语法/逻辑正确，存于 OPL 仓库根：
  - `smbman-fix-EMFILE-handle-leak.patch` — #1 SMB 根修 → `362053534/ps2sdk`（`smb_fio.c`），见第 3~6 节
  - `FatFs-PS2OPL-fix-EMFILE-handle-leak.patch` — #2 BDM 根修（推荐）→ `362053534/FatFs-PS2OPL@iop-r0.15`（`source/ff.c`），见第 9A 节
  - `bdmfs_fatfs-fix-EMFILE-handle-leak.patch` — #3 BDM 驱动侧防御（可选）→ `362053534/ps2sdk`（`fs_driver.c`），见第 9B 节
- 建议 PR：`fix/smbman-fdhandle-leak`（ps2sdk）、`fix/fclose-invalidate-on-error`（FatFs-PS2OPL）；可选 `fix/bdmfs-fatfs-fdhandle-leak`（ps2sdk）。ps2sdk 内的 #1/#3 可合成一个 PR，FatFs 的 #2 是独立仓库必须单独 PR。
- 至少做 **#1 + #2** 即可根治 SMB + BDM 两条链路；#3 视口味决定。
- 本会话 GitHub 令牌只覆盖 `362053534/Open-PS2-Loader`，无法 push `362053534/ps2sdk` 与 `362053534/FatFs-PS2OPL`（均 403，installation 只含 OPL 一个仓库）。用户已在 GitHub App 里加了 `ps2sdk` 的 Repository access，但令牌需 Arena 重新签发才生效；`FatFs-PS2OPL` 亦需一并加入。故 SDK/FatFs 提交改由**新会话**在令牌覆盖到这两个仓库后完成。
- 三份补丁都是 `git format-patch` 格式，`git am` 最省事；若行号/hash 对不齐，文档内附了修复后代码，可 `git apply --3way` 或手改（注意 `ff.c` 是 CRLF+Tab）。
- OPL 仓库侧的"过渡缓解"（对 smb 的 opendir 至少 readdir 一次）尚未实施；根治靠上述补丁，过渡缓解可选。

---

## 11. APA / HDD（PFS）链路：已核查，不受影响（无需改动）

用户追问内置硬盘的 **APA ISO** 与 **APA HDL** 两种模式是否也有同类泄漏。已核查：**没有，不需要打补丁。**

两种模式的 art / 配置读写最终都走 **PFS 文件系统驱动**（`pfs0:`，源码 `iop/hdd/pfs/src/`），它是与 smbman/bdmfs 对等的那一层。逐条比对结论：

- **槽位表**：`pfsFileSlots[]`，空闲判据 `.fd == NULL`，满了返回 `-EMFILE`（结构与 smbman/bdmfs 同构）。
- **关闭路径（关键，无泄漏）**：`pfs.c` 的 ops 表里 `close` 与 `dclose` **都指向 `pfsFioClose`** → `pfsFioCloseFileSlot()`；该函数**结尾无条件 `memset(fileSlot, 0, sizeof(pfs_file_slot_t))`**，对回写/`pfsCacheFlushAllDirty` 的返回值**直接忽略、不做错误跳转**。即**没有任何"出错就跳过释放"的分支**，槽位每次都被回收。
  - 对比：smbman 是 `if (r != 0) goto io_unlock;` 跳过 memset；FatFs 是仅在 `f_sync`+`validate` 成功时才清 `obj.fs`。PFS 两种坑都没有。
- **目录**：`pfsFioDopen` 复用 `pfsFioOpen`，`dclose` 复用 `pfsFioClose`，同样每次 memset 回收；无独立 dclose 漏点。
- **打开失败**：`openFile()` 出错时 `freeSlot->clink=NULL` 且 `pfsCacheFree(fileInode)`，槽位保持干净（`fd==NULL`）可复用，不半占用。
- **早返回**：`pfsFioClose` 若 `clink==NULL` 返回 `-EBADF`，那是槽位本就已释放，不是泄漏。

**为什么只有 SMB / BDM 中招：** ①它们的驱动在 close 错误路径上**跳过了释放**（smbman 的 `goto`、FatFs 的"仅成功才清"）；②SMB（网络）、BDM（USB/MX4SIO/SD 可插拔）本就易出瞬时错误，频繁踩中那条错误路径。PFS 既无"跳过释放"的分支，内置 APA HDD 又稳定，两头都不沾。

> 备注：PFS 的取舍是"忽略回写错误、强制释放槽位"（坏盘写入时可能**静默丢数据**），但这不影响只读的 art 浏览，也正是它不漏句柄的原因。

**故三条链路里只有 SMB + BDM 需要打补丁（#1 / #2 / #3），APA/HDD（PFS）无需改动。**

---

## 12. 句柄修复之后暴露出的"第二个"故障：art 加载全盘卡死（根因：smbman 缺接收超时）

### 12.1 现象（用户 ~290s 实机日志，build `...-51e1d54-main.elf`，SMB/eth + coverflow 主题）

- **句柄修复确认有效**：整段 290s 重度浏览**全程零 `errno=24`（EMFILE）**（旧问题 ~34s 必爆），现存 failure 全是 `errno=5`（真缺 art，正常）。
- **但出现一个此前被 EMFILE 崩溃掩盖住的、独立且更罕见的新故障**：某一刻起 loading 无限转圈、**所有封面停载变占位图**；按刷新**不再返回 0 个游戏**（游戏列表/SMB 会话仍活）、只是封面全空；**切换主题后封面加载恢复**。日志在 `[289.18] CONFIG No file smb:CFGSLPM_624.61.cfg.` 后戛然而止。

这**不是**句柄泄漏（fd 没耗尽），是另一类故障。

### 12.2 根因链（已逐层读码确认）

1. **单一全局 art I/O 锁**：`src/textures.c` 的 `texLoadAll()` 用**唯一**的 `fileLockId`（`textures.c:110/146`）把整段 `open + lseek + read + close`（479–512 行）全程锁住。所有 art（BG/COV/ICO/SCR…）都串行经过它。
2. **SMB read/open 无超时**：`texLoadAll` 里的 `open`/`read` 打到 `smb:` 时最终走 smbman(IOP) → SMSTCPIP。**smbman 的数据 socket 没有设置接收超时**，网络一打嗝，那次 `read`（或 `open`）就**永久阻塞**、`SignalSema(fileLockId)` 永远执行不到。
3. **全局锁被永久占住** → 其余两个 worker 及后续所有 art 加载全部堵在 `WaitSema(fileLockId)` → **所有封面停载变占位图**。
4. **派发闸门永久关闭（不自愈的关键）**：`src/texcache.c` dispatch 的门控是 `if (!reqN.qr)`（COV 在 684 行）。worker 只在 `itemGetImage` **返回之后**才执行 `ioReq->qr = 0`（`texcache.c:299`）。卡死的 worker 永远清不掉 `req2.qr=1`，于是**即便锁被放开，COV 这条队列也永久派发不出新图**。
5. **游戏列表为何还活**：列表走 `sbReadList` 的 `opendir/readdir`，**完全不经过 `texLoadAll`/`fileLockId`**，故刷新不归零、会话不死。
6. **切主题为何能恢复**：切主题触发 cache 的 deinit/reinit —— `DeleteSema(fileLockId)`+`CreateSema`（`textures.c:146/151`）建了一把全新锁，并复位 `forceSkipQr`/`reqN.qr` 门控，新加载得以继续（那个卡死的 worker/syscall 被泄漏掉了，但功能上恢复）。

**旁证**：`src/texcache.c:307–333` 有一段作者**已注释掉的看门狗**（"加载超 10s 无按键就 `pthread_cancel`+重建三 worker+`texLoading=0`"），并自注"补救措施，大概率没用作用"——正说明作者早知有"加载卡死"故障，也知道 cancel+重建救不了（线程卡在 syscall 里、锁放不出来）。**用户已明确要求不做 EE 侧兜底**（怕副作用），故此看门狗保持注释、不启用。

### 12.3 根治位置（与 EMFILE / BDM 同一模式：根在 IOP 驱动，EE 只是暴露它）

**在 smbman（`362053534/ps2sdk`，`iop/network/smbman/src/`）的数据连接建立后给 socket 设接收超时**，让卡死的 `read` 超时返回错误，而非永久阻塞。链路自愈：`read` 超时 → `texLoadAll` 返回 `ERR_BAD_FILE` 并 `SignalSema(fileLockId)` 放锁 → worker 走到 `ioReq->qr = 0` → 该封面标记失败、其余封面继续加载，不再全盘冻结。

**可行性已核实**：OPL 自带的 SMSTCPIP **支持 `SO_RCVTIMEO`**——`sockets.c:1361` 写入 `conn->recv_timeout`，并在 `api_lib.c:436/482` 的 `sys_arch_mbox_fetch(..., conn->recv_timeout)` 中真正生效（超时返回 `SYS_ARCH_TIMEOUT`）。因此 smbman 侧 `setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, ...)` 即可根治。

> 待新会话在 ps2sdk 令牌覆盖到位后落实：找到 smbman 建立 TCP 数据连接（connect 成功）之后的位置，加 `SO_RCVTIMEO`（并视需要 `SO_SNDTIMEO`），超时值取一个既能容忍正常大图传输、又能及时判死的量级（如 10s，可再调）。同时确认 smb_read 对 recv 返回负值/超时的错误处理会把错误一路上报到 EE 的 `read()`。

### 12.4 本会话已加的定向诊断日志（排查用，事后按惯例回退，只留诊断期）

目的：实机复现时**钉死是哪条 art、卡在 open 还是 read、以及门控是否卡住**。

- **`src/textures.c` `texLoadAll`**：在 `open` 前后、`read` 前后各打一对"进入/返回"标记：
  - `texLoadAll: >>> open ENTER path=...` / `texLoadAll: <<< open OK fd=... path=...`
  - `texLoadAll: >>> read ENTER size=... path=...` / `texLoadAll: <<< read OK size=... path=...`
  - **判读**：若某条只出现 `>>> ... ENTER` 而无对应 `<<< ... OK`、且其后日志停住，即该次 I/O 卡死点（并区分 open vs read、定位到具体文件）。
- **`src/texcache.c` `flushBatchRequests`（每帧调一次）**：`texLoading>0` 时每 120 帧(~2s)打一次门控快照，回落 0 即复位：
  - `texcache DIAG: texLoading=.. req1(BG).qr=.. req2(COV).qr=.. req3(ICO).qr=.. frames=..`
  - **判读**：卡死时可见 `texLoading` 长期 >0、某个 `reqN.qr` 恒为 1、`frames` 持续上涨——直接指认卡死的 worker（BG=1/COV=2/ICO=3）。

> 这些仅诊断，不改变任何行为逻辑；根因锁定后连同前述排查日志一并回退。**未做**任何 EE 侧兜底/看门狗（用户明确要求）。

## 13. 【修正并根治】封面全盘停载的真因 = EE 侧 `texLoading` 计数泄漏（非 smbman 超时）

> 本节**修正第 12 节的根因判断**。第 12 节把"封面全停"归因于 smbman 无接收超时导致 `read` 永久阻塞、全局 `fileLockId` 被占死。用户随后给出**可靠复现步骤**并附实机日志（~2942–2977s），据此逐层读码后确认：**第 12 节现象的真正原因是 EE 侧 texcache 的 `texLoading` 计数泄漏，与 I/O 阻塞无关**。smbman 超时补丁仍是"若真的发生 read 阻塞"时的有益加固（第 12 节保留），但它**不是本故障的根因**，单独打它也修不好本故障。

### 13.1 可靠复现（用户提供）

1. 在游戏列表按 **方块** 进入某游戏的 **INFO 详情页**；
2. 在详情页内用 **UP/DOWN 反复切换游戏浏览**（会持续加载 SCR/SCR2 截图）；
3. 按 **圈** 返回游戏列表 → **大概率所有封面加载全停**；
4. 此后**按刷新也不恢复**；**切换主题才恢复**。

### 13.2 关键日志事实（推翻 smbman-阻塞假设）

- **没有任何阻塞 I/O**：`texLoadAll` 的每个 `>>> open ENTER` 都有 `<<< open OK`，每个 `>>> read ENTER` 都有 `<<< read OK`（BG/SCR/SCR2 全部读完）。SMB 读**没卡**。
- **`texcache DIAG` 显示计数泄漏**：卡死期每行恒为 `texLoading=N(>0) req1(BG).qr=0 req2(COV).qr=0 req3(ICO).qr=0`，`frames` 持续上涨。即**三个 pthread worker 全空闲(qr=0)，但 texLoading 永久卡在正值**（观测到 1/2/3/5/6/8/9/11 等）——`++` 漏了对应的 `--`。

### 13.3 根因（已读码坐实）

`src/texcache.c` 的 `cacheGetTexture()`：非 BG/COV/ICO 后缀的 art（**典型就是 INFO 详情页的 SCR/SCR2 截图**）即使在 `usePthread` 模式下也走**官方 IO 队列**（`IO_CACHE_LOAD_ART`），落在那段 `else` 分支。该分支旧代码**手写**了加载逻辑：

```c
texLoading++;                                   // 先增
req = calloc(...); req->qr = 1;
ioPutRequest(IO_CACHE_LOAD_ART, req);           // ← 忽略返回值！
```

IO 请求池是**定长的**（`src/ioman.c`：`#define MAX_IO_REQUESTS 16`，`AllocIoRequest()` 满则返回 NULL、`ioPutRequest` 返回 `IO_ERR_IO_BLOCKED`）。在 INFO 页快速上下浏览、同时 coverflow 预取也在抢占请求槽时，**请求池被占满** → `ioPutRequest` 失败 → **请求既没入队、其处理函数 `cacheLoadImage1` 也永不执行** → 这次的 `texLoading++` 再也没有对应的 `--` → **`texLoading` 只增不减地永久泄漏**（同时泄漏 `calloc` 出的 `req`、并把该缓存槽 `qr` 永久钉在 1）。

**泄漏 → 封面全停的机理**：`texLoading` 一旦卡在 >0，`cacheGetTexture` 顶部的门控 `if (curStartUp != value)` → `if (curStartUp && !ForceRefreshPrevTexCache && texLoading > 0)` 会在**每次光标移动**时置 `cdFramesCount=1`/`skipQr=1`，随后 `if (skipQr) return ...` 直接返回、**不再派发任何新封面加载**。刷新不清 `texLoading`（故不恢复）；切主题触发 cache teardown/reinit，`texLoading` 与门控被重置（故恢复）。**与实机现象逐条吻合。**

**为何是"非对称 bug"**：同一文件另有两处入队 `IO_CACHE_LOAD_ART` 的调用都**正确校验了返回值并回滚**——`cacheQueueImageRequest()`（`src/texcache.c:169`，`!= IO_OK` 时调 `cacheCancelImageRequest` 回滚 `texLoading`/槽位/内存），以及 `!usePthread` 分支和 coverflow 的 `cacheGetTextureQuiet()` 都走 `cacheQueueImageRequest()`。唯独这段 `else` 分支是手写的漏检副本。

### 13.4 修复（本会话已改并推送，commit `67069fe`）

把该 `else` 分支的手写加载逻辑整段替换为调用现成的 `cacheQueueImageRequest(cache, *cacheId, list, value, itemId)`，与 `!usePthread` 分支完全一致。该 helper 对 `calloc` 失败与 `ioPutRequest` 失败都会**同步回滚** `texLoading` 与槽位状态（`cacheCancelImageRequest`），从根本上杜绝泄漏。改后全文件仅剩 `cacheQueueImageRequest` 内一处带返回值校验的 `IO_CACHE_LOAD_ART` 入队点。

- **改动文件**：`src/texcache.c`（仅此一处，纯 EE 侧）。
- **不涉及** IOP/SDK：本故障根在 OPL 的 EE 代码，**不需要**改 ps2sdk/smbman 即可修复（与 EMFILE/BDM 那类"根在 IOP 驱动"的问题不同）。第 12 节的 smbman `SO_RCVTIMEO` 加固仍建议做，但定位为"真发生网络阻塞时的防御"，与本故障相互独立。

### 13.5 待用户实机验证

1. 用含本 commit 的 build 复现 13.1 的步骤（INFO 页反复浏览 → 圈返回）；
2. 观察 `texcache DIAG`：`texLoading` 应能在浏览停止后**回落到 0**，不再恒为正值；封面加载不再全停、刷新即恢复。
3. 验证通过后，按用户既定惯例（第二十三问）**回退第 12.4 节所列的排查诊断日志**（`textures.c` 的 open/read ENTER/OK、`texcache.c` 的 `texcache DIAG`、`cacheLoadImage FAILED`），只保留本修复。回退前建议保留一次带日志的复现结果存档。
