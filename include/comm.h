#ifndef _SWAYLOCK_COMM_H
#define _SWAYLOCK_COMM_H

#include <stdbool.h>

struct swaylock_password;

bool spawn_comm_child(void);
ssize_t read_comm_request(char **buf_ptr);
bool write_comm_reply(bool success);
// Requests the provided password to be checked. The password is always cleared
// when the function returns.
bool write_comm_request(struct swaylock_password *pw);
bool read_comm_reply(bool *auth_success);
// FD to poll for password authentication replies.
int get_comm_reply_fd(void);

/* Set the close-on-exec flag for `fd` (implemented in main.c; also declared in
 * swaylock.h — repeated here so fprint.c does not need the UI header). */
bool set_cloexec(int fd);

/* Bytes exchanged with the swaylock-sleep-watcher helper. The helper writes
 * enter/resume bytes to the parent; the parent replies with SLEEP_COMM_ACK
 * once it has prepared for sleep. */
#define SLEEP_COMM_ENTER 'S'
#define SLEEP_COMM_RESUME 'R'
#define SLEEP_COMM_ACK 'A'

#endif
