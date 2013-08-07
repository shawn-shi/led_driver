#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>

int main(int argc, char *argv[])
{
	int fd;
	unsigned char key;
	int nread;
	
	fd = open("/dev/button", O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		printf("open button device failed.\n");
		return -1;
	}

	while (1) {
		read(fd, &key, sizeof(key));
		printf("key = %#x\n", key);
	}
	
	close(fd);
	return 0;
}
