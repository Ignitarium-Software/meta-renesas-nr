#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#define RPMSG_SYSFS_PATH "/sys/bus/rpmsg/devices/"
#define RPMSG_EP_SERVICE "virtio0.rpmsg-client-sample"

int get_endpoint_info(int *dst_addr)
{
    DIR *dp;
    struct dirent *entry;

    char device_path[512];
    char dst_path[600];
    char buffer[128];

    FILE *fp;

    dp = opendir(RPMSG_SYSFS_PATH);
    if (!dp) {
        perror("opendir failed");
        return 1;
    }

    while ((entry = readdir(dp)) != NULL) {

        if (strncmp(entry->d_name, RPMSG_EP_SERVICE, strlen(RPMSG_EP_SERVICE)) == 0) {

            /* Build full device path */
            snprintf(device_path, sizeof(device_path),
                     "%s%s", RPMSG_SYSFS_PATH, entry->d_name);

            /* Build dst file path */
            snprintf(dst_path, sizeof(dst_path),
                     "%s/dst", device_path);

            printf("Opening: %s\n", dst_path);

            fp = fopen(dst_path, "r");
            if (!fp) {
                perror("fopen failed");
                closedir(dp);
                return 1;
            }

            if (fgets(buffer, sizeof(buffer), fp) == NULL) {
                perror("fgets failed");
                fclose(fp);
                closedir(dp);
                return 1;
            }

            fclose(fp);

            /* Remove newline if present */
            buffer[strcspn(buffer, "\n")] = 0;

            /* Convert to integer safely */
            char *endptr;
            long dst_value = strtol(buffer, &endptr, 0);

            if (endptr == buffer) {
                printf("Invalid number in dst: %s\n", buffer);
                closedir(dp);
                return 1;
            }

			*dst_addr = dst_value;

            closedir(dp);
            return 0;
        }
    }

    printf("No matching RPMsg device found\n");

    closedir(dp);
    return 1;
}
