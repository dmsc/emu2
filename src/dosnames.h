
// DOS file names to Unix file names conversions

#ifndef DOSNAMES_H
#define DOSNAMES_H

#include <stdint.h>

// Converts a DOS full path to equivalent Unix filename
// If the file exists, returns the name of the file.
// If the file does not exists, and "force" is true, returns the possible lowercase name.
// If the file does not exists, and "force" is false, returns null.
char *dos_unix_path(int addr, int force);

// Converts a DOS FCB file name to equivalent Unix filename
// If the file exists, returns the name of the file.
// If the file does not exists, and "force" is true, returns the possible lowercase name.
// If the file does not exists, and "force" is false, returns null.
char *dos_unix_path_fcb(int addr, int force);

// Changes current working directory
int dos_change_cwd(char *path);
int dos_change_dir(int addr);

// Returns a DOS path representing given Unix path in drive
char *dos_real_path(char drive, const char *unix_path);

// Gets current working directory
const uint8_t *dos_get_cwd(int drive);

// Sets/gets default drive
void dos_set_default_drive(int drive);
int dos_get_default_drive(void);

// Struct used as return to dosFindFirstFile
struct dos_file_list
{
    uint8_t dosname[13];
    char *unixname;
};

// Implements "find first file" and "next file" DOS functions.
// Returns a list with filenames compatibles with the DOS filespec, as pairs
// The list will be deleted at the next call
struct dos_file_list *dos_find_first_file(int addr);
struct dos_file_list *dos_find_first_file_fcb(int addr);

// Frees a fileList.
void dos_free_file_list(struct dos_file_list *dl);

#endif // DOSNAMES_H
