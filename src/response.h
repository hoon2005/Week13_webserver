#ifndef RESPONSE_H
#define RESPONSE_H

int response_send_error(int fd, int status, const char *reason,
                        const char *message);
int response_send_static(int fd, const char *path);

#endif
