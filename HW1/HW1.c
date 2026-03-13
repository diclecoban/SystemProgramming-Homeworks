#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <ctype.h>

struct criteria {
    char *path;
    char *filename;
    long size;
    char type;
    char *permissions;
    int nlinks;
};

int check_criteria(struct stat* file_stat, struct criteria* my_criteria, const char* filename);
void print_result(const char* name, int depth);
void search(const char *path, struct criteria* my_criteria, int depth, int *found);

void signal_handler(int signal) {
    fprintf(stderr, "Returning all resources to the system and exit the program.");
    exit(0);
}

void init_criteria(struct criteria *my_criteria) {
    my_criteria->path = NULL;
    my_criteria->filename = NULL;
    my_criteria->size = -1;
    my_criteria->type = '\0';
    my_criteria->permissions = NULL;
    my_criteria->nlinks = -1;
}

void parse_criteria(int arg_count, char *arg_list[], struct criteria *my_criteria){
    int option;
    while((option = getopt(arg_count, arg_list, "w:f:b:t:p:l:")) != -1){
        switch(option) {
            case 'w': my_criteria->path = optarg; break;
            case 'f': my_criteria->filename = optarg; break; 
            case 'b': my_criteria->size = atoi(optarg); break; //atoi = ascii to integer
            case 't': my_criteria->type = optarg[0]; break;
            case 'p': 
                if(strlen(optarg) != 9) {
                    fprintf(stderr, "Invalid permissions format. Use 9 characters (e.g. rwxr-xr--)\n");
                    exit(1);
                } // I add it if the input would be "rwx" like this.
                my_criteria->permissions = optarg; break;
            case 'l':my_criteria->nlinks = atoi(optarg); break;
            case '?': fprintf(stderr, "Invalid criteria.\n"); break;
        }
    }
}

int match_type(struct stat *file_type, struct criteria* my_criteria) {
    switch (my_criteria->type) {
        case 'd': return S_ISDIR(file_type->st_mode); 
        case 's': return S_ISSOCK(file_type->st_mode);
        case 'b': return S_ISBLK(file_type->st_mode);
        case 'c': return S_ISCHR(file_type->st_mode);
        case 'f': return S_ISREG(file_type->st_mode);        
        case 'p': return S_ISFIFO(file_type->st_mode);
        case 'l': return S_ISLNK(file_type->st_mode);
        default:  return 0;
    }
}

// my_criteria (p) is pattern, file_stat (s) is string
int match_filename(struct criteria* my_criteria, const char* filename) {
    char *pattern = my_criteria->filename;
    const char *string = filename;

    while (*pattern != '\0') {
        if (*(pattern + 1) == '+') {
            if (tolower(*pattern) != tolower(*string)) return 0;
            string++;
            while (*string != '\0' && tolower(*pattern) == tolower(*string))
                string++;
            pattern += 2; 
        } else {
            if (tolower(*pattern) != tolower(*string)) return 0;
            pattern++;
            string++;
        }
    }
    return (*string == '\0') ? 1 : 0;
}

int match_size(struct stat* file_stat, struct criteria* my_criteria){
    if(file_stat->st_size == my_criteria->size) {
        return 1;
    }

    return 0;
}

int match_nlinks(struct stat* file_stat, struct criteria* my_criteria) {
    if(file_stat->st_nlink == my_criteria->nlinks) {
        return 1;
    }
    return 0;
}


void parse_permission(char permission[], int bitPermission[]) {

    for(int i=0; i<9; i++) {
        if(permission[i] == '-') {
            bitPermission[i] = 0;
        } else {
            bitPermission[i] = 1;
        }
    }
}

int match_permission(struct stat* file_stat, struct criteria* my_criteria) {

int bitPermission[9];
parse_permission(my_criteria->permissions, bitPermission);

int macros[] = {S_IRUSR, S_IWUSR, 
S_IXUSR, S_IRGRP, S_IWGRP, S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH};

    for(int i=0; i<9; i++) {
        if(bitPermission[i] == 1){
            if((file_stat->st_mode & macros[i]) == 0) {
               return 0;
            }
        } else if(bitPermission[i] == 0) {
            if((file_stat->st_mode & macros[i]) != 0) {
                return 0;
            }
        }
    }

    return 1;
}

void print_result(const char* name, int depth) {
    printf("|");
    for(int i=0; i < depth*4 - 2; i++) {
        printf("-");
    }
    printf("%s\n", name);
}

int check_criteria(struct stat* file_stat, struct criteria* my_criteria, const char* filename) {
    if(my_criteria->size != -1 && 
        match_size(file_stat,my_criteria) == 0 ) {
            return 0;
    } if(my_criteria->filename != NULL && 
        match_filename(my_criteria, filename) == 0) {
            return 0;
    } if(my_criteria->type != '\0' && 
        match_type(file_stat, my_criteria) == 0) {
            return 0;
    } if(my_criteria->permissions != NULL &&
        match_permission(file_stat, my_criteria) == 0) {
            return 0;
    } if(my_criteria->nlinks != -1 && 
        match_nlinks(file_stat, my_criteria) == 0) {
            return 0;
    }
    return 1;
}


void search(const char *path, struct criteria* my_criteria, int depth, int *found) {
    char full_path[1024];
    DIR *dir = opendir(path);

    if(dir == NULL) {
    fprintf(stderr, "Dir can't open: %s\n", path);
    return;
    }

    struct dirent *entry;

    while((entry = readdir(dir)) != NULL) {
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        int len = snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        if (len >= sizeof(full_path)) {
            fprintf(stderr, "Path too long: %s/%s\n", path, entry->d_name);
            continue;
        } // I add it just in case
        struct stat file_stat;

        if (lstat(full_path, &file_stat) == -1) {
            fprintf(stderr, "lstat failed: %s\n", full_path);
            continue;
        }

        if(check_criteria(&file_stat, my_criteria, entry->d_name)) {
            print_result(entry->d_name, depth);
            (*found)++;
        }
        if(S_ISDIR(file_stat.st_mode)) {
            search(full_path, my_criteria,depth+1, found);
        }
    }
    if (closedir(dir) == -1) {
        fprintf(stderr, "closedir failed: %s\n", path);
    } // I add the system call checking just in case. 
}

int main(int arg_count, char* arg_list[]) {
    signal(SIGINT, signal_handler);
    int found = 0;
    struct criteria my_criteria;
    init_criteria(&my_criteria);

    parse_criteria(arg_count, arg_list, &my_criteria);

    if(my_criteria.path == NULL) {
        fprintf(stderr, "Usage: ./myFind -w path [-f filename] [-b size] [-t type] [-p permissions] [-l nlinks]\n");
        return 1;
    }
    search(my_criteria.path, &my_criteria, 1, &found);

    if(found == 0) {
        printf("No file found!\n");
    }
    return 0;
}