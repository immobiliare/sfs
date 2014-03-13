#ifndef UTIL_LINUX_SETPROCTITLE_H
#define UTIL_LINUX_SETPROCTITLE_H

void initproctitle (int argc, char **argv);
void setproctitle (const char *prog);

#endif