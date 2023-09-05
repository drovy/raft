/* Sequential writes using io_uring. */

#ifndef DISK_URING_H_
#define DISK_URING_H_

#include <stddef.h>
#include <sys/uio.h>

#include "fs.h"
#include "report.h"
#include "tracing.h"

int DiskWriteUsingUring(int fd,
                        struct iovec *iov,
                        unsigned n,
                        struct FsFileInfo *info,
                        struct Tracing *tracing,
                        struct histogram *histogram);

#endif /* DISK_URING_H_ */
