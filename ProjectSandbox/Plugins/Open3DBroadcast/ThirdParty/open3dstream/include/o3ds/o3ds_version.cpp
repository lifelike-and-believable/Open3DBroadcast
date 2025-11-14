#include "o3ds_version.h"

#define QUOTE_(str) #str
#define QUOTE(str) QUOTE_(str)

const char* O3DS::getVersion()
{
	return QUOTE(O3DS_VERSION_TAG);
}
