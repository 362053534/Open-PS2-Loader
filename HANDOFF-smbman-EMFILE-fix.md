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
```
