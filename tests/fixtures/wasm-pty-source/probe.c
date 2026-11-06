#include <unistd.h>
#include <poll.h>
#include <sys/select.h>
int main(int argc, char** argv) { if (argc == 12345) { char c; struct pollfd p = {0, POLLIN, 0}; fd_set f; FD_ZERO(&f); FD_SET(0, &f); struct timeval t = {0, 0}; read(0, &c, 1); poll(&p, 1, 0); select(1, &f, 0, 0, &t); } return 0; }
