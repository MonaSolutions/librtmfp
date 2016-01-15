#pragma once

#include <stdio.h>

// Multiplaform function to open a file
unsigned short openFile(FILE** pFileObj, const char* path, const char* flags) {
#if defined(WIN32)
	errno_t err;
	if ((err = fopen_s(pFileObj, path, flags)) != 0) {
		printf("Unable to open file %s : %d\n", path, err);
		return 0;
	}
#else
	if ((*pFileObj = fopen(path, flags)) == NULL) {
		printf("Unable to open file %s\n", path);
		return 0;
	}
#endif
	return 1;
}
