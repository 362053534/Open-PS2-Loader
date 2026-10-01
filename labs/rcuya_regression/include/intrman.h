#ifndef RCUYA_TEST_INTRMAN_H_
#define RCUYA_TEST_INTRMAN_H_

int CpuSuspendIntr(int *state);
int CpuResumeIntr(int state);

#endif
