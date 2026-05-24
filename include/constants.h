// SPDX-License-Identifier: GPL-3.0-only

#ifndef CONSTANTS_H
#define CONSTANTS_H

/* Universal error / return codes.
   Uses POSIX errno convention (positive values).
   Use `-EINVAL` etc. to return errors from functions. */
#define SUCCESS 0
#define EINVAL 22
#define ENODEV 19
#define EIO 5
#define ENOENT 2
#define ENOTDIR 20
#define EMFILE 24
#define EBADF 9
#define ECHILD 10
#define EACCES 13
#define ENOMEM 12

#endif /* CONSTANTS_H */
