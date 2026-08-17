#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#define RPMSG_SYSFS_PATH "/sys/bus/rpmsg/devices/"
#define MAX_VIRTIO_BUS 2

int get_endpoint_info(int *dst_src, int *dst_addr, int *virtio_id)
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

		for (int virtio_no = 0; virtio_no < MAX_VIRTIO_BUS; virtio_no++) {

			char virtio_ep_service_path[64];

			snprintf(virtio_ep_service_path, sizeof(virtio_ep_service_path),
					"%s%d%s","virtio", virtio_no, ".tuning_ep");

			if (strncmp(entry->d_name, virtio_ep_service_path, strlen(virtio_ep_service_path)) == 0) {

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

				/* Build src file path */
				snprintf(dst_path, sizeof(dst_path),
						"%s/src", device_path);

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
				dst_value = strtol(buffer, &endptr, 0);

				if (endptr == buffer) {
					printf("Invalid number in src: %s\n", buffer);
					closedir(dp);
					return 1;
				}

				*dst_src = dst_value;

				*virtio_id = virtio_no;
				closedir(dp);
				return 0;
			}
		}
	}

	printf("No matching RPMsg device found\n");

	closedir(dp);
	return 1;
}
