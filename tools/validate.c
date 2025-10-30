#include "validate.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/cfgchk.h"
#include "log.h"

int zone_validate_command(int argc, char **argv) {
    struct cfgchk_request req;
    int fd, ret;

    (void)argc;
    (void)argv;

    memset(&req, 0, sizeof(req));
    req.version = CFGCHK_IOCTL_VERSION;

    fd = open("/dev/hvisor_cfgchk", O_RDWR);
    if (fd < 0) {
        log_error("Failed to open /dev/hvisor_cfgchk (%s)", strerror(errno));
        return -1;
    }

    ret = ioctl(fd, HVISOR_CFG_VALIDATE, &req);
    close(fd);
    if (ret != 0) {
        log_error("Kernel validation interface returned error (errno=%d: %s)",
                  errno, strerror(errno));
        return -1;
    }

    printf("[OK] cfgchk communication success.\n");
    return 0;
}
