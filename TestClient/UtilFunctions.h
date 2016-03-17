#pragma once

#include <stdio.h>

#if defined(WIN32)
	#include <share.h>
	#undef fopen
	#define fopen(PATH,FLAGS) _fsopen(PATH, FLAGS, _SH_DENYNO)
#endif

// Multiplaform function to open a file
unsigned short openFile(FILE** pFileObj, const char* path, const char* flags) {
	if ((*pFileObj = fopen(path, flags)) == NULL) {
		printf("Unable to open file %s\n", path);
		return 0;
	}
	return 1;
}
