#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <signal.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>


#define MAX_REQ 4096
#define MAX_PATH 1024
#define MAX_FILES 1000
#define PORT 8080
int dev_mode=0, use_fork=0, use_lua=0;
unsigned char *zip_data = NULL;
size_t zip_size = 0;
long zip_start_offset = 0; 

lua_State* init_lua()
{
    lua_State *L = luaL_newstate();
    if(!L) return NULL;
    luaL_requiref(L, "_G", luaopen_base, 1); 
    lua_pop(L,1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table,1);
    lua_pop(L,1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); 
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); 
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1); 
    lua_pop(L, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1); 
    lua_pop(L, 1);

    lua_pushnil(L); lua_setglobal(L,"io");
    lua_pushnil(L); lua_setglobal(L,"os");
    lua_pushnil(L); lua_setglobal(L,"package");
    lua_pushnil(L); lua_setglobal(L,"debug");

    return L;
}
typedef struct {
    char filename[MAX_PATH];
    long local_header_offset; 
    int size;
    int cmpr_size;
    int cmpr_method;
} zip_entry_t;

zip_entry_t zip_contents[MAX_FILES];
int zip_entry_cnt = 0;

const unsigned char *extract_file_data(const zip_entry_t *entry, size_t *out_size) {
    if ((entry->cmpr_method != 0) && (dev_mode)) {
        printf("DEBUG: File '%s' is compressed (method %d), skipping\n", 
               entry->filename, entry->cmpr_method);
        return NULL;
    }
    
    const unsigned char *local_header = zip_data + entry->local_header_offset;
    if (memcmp(local_header, "PK\003\004", 4) != 0) {
        if (dev_mode) printf("DEBUG: Invalid local file header for '%s'\n", entry->filename);
        return NULL;
    }
    
    unsigned short local_name_len = *(unsigned short *)(local_header + 26);
    unsigned short local_extra_len = *(unsigned short *)(local_header + 28);
    const unsigned char *file_data = local_header + 30 + local_name_len + local_extra_len;
    
    *out_size = entry->size;
    return file_data;
}

void discover_zip_structure() {
    if (dev_mode){
    printf("DEBUG: Discovering ZIP structure...\n");
    if (zip_size < 22) {
        printf("DEBUG: ZIP too small\n");
        return;
    }}
    
   // search from end
    const unsigned char *eocd = NULL;
    for (long i = zip_size - 22; i >= 0; i--) {
        if (memcmp(zip_data + i, "PK\005\006", 4) == 0) {
            // validate EOCD
            unsigned short comment_len = *(unsigned short *)(zip_data + i + 20);
            if (i + 22 + comment_len == zip_size) {
                eocd = zip_data + i;
                break;
            }
        }
    }
    if (!eocd) {
        if (dev_mode)
        printf("DEBUG: No valid EOCD found in ZIP data\n");
        return;
    }
    
    //OFFSETS ARE RELATIVE TO ZIP START
    unsigned short total_entries = *(unsigned short *)(eocd + 10);
    unsigned int cd_offset = *(unsigned int *)(eocd + 16); 
    if ((cd_offset >= zip_size)) {
        if(dev_mode)
        printf("DEBUG: Central directory offset %u is beyond ZIP size %lu\n", cd_offset, zip_size);
        return;
    }
    const unsigned char *cd_ptr = zip_data + cd_offset; 
    zip_entry_cnt = 0;
    for (int i = 0; i < total_entries && zip_entry_cnt < MAX_FILES; i++) {
        if ((cd_ptr + 46) > (zip_data + zip_size)) {
            if(dev_mode)
            printf("DEBUG: Central directory entry %d extends beyond ZIP\n", i);
            break;
        }
        
        if ((memcmp(cd_ptr, "PK\001\002", 4) != 0)) {
            if(dev_mode){
            printf("DEBUG: Invalid central directory entry signature at entry %d\n", i);
            printf("DEBUG: Found bytes: %02x %02x %02x %02x\n", 
                   cd_ptr[0], cd_ptr[1], cd_ptr[2], cd_ptr[3]);
            break;
            }
        }
        unsigned short cmpr_method = *(unsigned short *)(cd_ptr + 10);
        unsigned int cmpr_size = *(unsigned int *)(cd_ptr + 20);
        unsigned int uncmpr_size = *(unsigned int *)(cd_ptr + 24);
        unsigned short filename_len = *(unsigned short *)(cd_ptr + 28);
        unsigned short extra_len = *(unsigned short *)(cd_ptr + 30);
        unsigned short comment_len = *(unsigned short *)(cd_ptr + 32);
        unsigned int local_header_offset = *(unsigned int *)(cd_ptr + 42); // Relative to ZIP start
        
        if (((cd_ptr + 46 + filename_len) > (zip_data + zip_size))) {
            if (dev_mode)
            printf("DEBUG: Filename extends beyond ZIP for entry %d\n", i);
            break;
        }
        if (filename_len > 0 && filename_len < MAX_PATH) {
            strncpy(zip_contents[zip_entry_cnt].filename, (char *)(cd_ptr + 46), filename_len);
            zip_contents[zip_entry_cnt].filename[filename_len] = '\0';
            zip_contents[zip_entry_cnt].local_header_offset = local_header_offset;
            zip_contents[zip_entry_cnt].size = uncmpr_size;
            zip_contents[zip_entry_cnt].cmpr_size = cmpr_size;
            zip_contents[zip_entry_cnt].cmpr_method = cmpr_method;
            
            //skip entries ending with '/'
            if ((zip_contents[zip_entry_cnt].filename[filename_len - 1] != '/')){
                if (dev_mode)printf("DEBUG: Found: '%s' (size: %u, method: %d, offset: %u)\n", 
                       zip_contents[zip_entry_cnt].filename, uncmpr_size, 
                       cmpr_method, local_header_offset);
                zip_entry_cnt++;
            } else {
                if(dev_mode)
                printf("DEBUG: Skipping directory: '%s'\n", zip_contents[zip_entry_cnt].filename);
            }
        }
        cd_ptr += 46 + filename_len + extra_len + comment_len;
}
}

const zip_entry_t* find_zip_entry(const char *path) {
    for (int i = 0; i < zip_entry_cnt; i++) {
        if (strcmp(zip_contents[i].filename, path) == 0) {
            return &zip_contents[i];
        }
    }
    return NULL;
}

const zip_entry_t* find_best_match(const char *requested_path) {
    const zip_entry_t *entry = find_zip_entry(requested_path);
    if (entry) return entry;
    if (strlen(requested_path) == 0) {
        const char *index_files[] = {"index.html", "index.htm", "default.html", "default.htm"};
        for (int i = 0; i < 4; i++) {
            entry = find_zip_entry(index_files[i]);
            if (entry) return entry;
        }
        for (int i = 0; i < zip_entry_cnt; i++) {
            const char *filename = strrchr(zip_contents[i].filename, '/');
            if (filename) filename++; else filename = zip_contents[i].filename;
            
            if (strcmp(filename, "index.html") == 0 || strcmp(filename, "index.htm") == 0) {
                return &zip_contents[i];
            }
        }
    }
    for (int i = 0; i < zip_entry_cnt; i++) {
        const char *filename = strrchr(zip_contents[i].filename, '/');
        if (filename) filename++; else filename = zip_contents[i].filename;
        
        if (strcmp(filename, requested_path) == 0) return &zip_contents[i];
     } return NULL;
}

const char* guess_content_type(const char *path) {   // libmagic, magika
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".xml") == 0) return "application/xml";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcmp(ext, ".woff") == 0) return "font/woff";
    if (strcmp(ext, ".woff2") == 0) return "font/woff2";
    if (strcmp(ext, ".ttf") == 0) return "font/ttf";
    if (strcmp(ext, ".otf") == 0) return "font/otf";
    if (strcmp(ext, ".mp4") == 0) return "video/mp4";
    if (strcmp(ext, ".webm") == 0) return "video/webm";
    if (strcmp(ext, ".pdf") == 0) return "application/pdf";
    if (strcmp(ext, ".txt") == 0) return "text/plain";
    if (strcmp(ext, ".wasm") == 0) return "application/wasm";
    
    return "application/octet-stream";
}

void serve_path(int client_fd, const char *url_path) {
    if (strstr(url_path, "..")) {
        const char *err = "HTTP/1.1 400 Bad Request\r\n\r\nInvalid request path";
        write(client_fd, err, strlen(err));
        return;
    }
    char clean_path[MAX_PATH];
    if (url_path[0] == '/') {
        strcpy(clean_path, url_path + 1);
    } else {
        strcpy(clean_path, url_path);
    }
    const zip_entry_t *entry = find_best_match(clean_path);
    if (!entry) {
        if (dev_mode)
        printf("DEBUG: No file found, sending 404\n");
        const char *response = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                              "<html><body><h1>404 Not Found</h1><p>Available files:</p><ul>";
        write(client_fd, response, strlen(response));
        for (int i = 0; i < zip_entry_cnt && i < 10; i++) {
            char file_info[512];
            snprintf(file_info, sizeof(file_info), "<li>%s</li>", zip_contents[i].filename);
            write(client_fd, file_info, strlen(file_info));
        }
        const char *footer = "</ul></body></html>";
        write(client_fd, footer, strlen(footer));
        return;
    }
    size_t file_size;
    const unsigned char *file_data = extract_file_data(entry, &file_size);

    if (!file_data) {
        if(dev_mode)
        printf("DEBUG: Could not extract file data, sending 500\n");
        const char *response = "HTTP/1.1 500 Internal Server Error\r\n\r\nCould not extract file";
        write(client_fd, response, strlen(response));
        return;
    }
    if(use_lua && strstr(entry->filename,".lua")){
        lua_State *L = init_lua();
        if(!L){
            const char *err = "HTTP/1.1 500 Internal Server Error\r\n\r\nFailed to init Lua";
            write(client_fd,err,strlen(err));
            return;
        }
        char *query_start=strchr(url_path, '?');
        lua_newtable(L);
        lua_pushstring(L, url_path);
        lua_setfield(L,-2,"path");
        // request.method (always GET or HEAD for now)
        lua_pushstring(L,"GET");
        lua_setfield(L,-2, "method");
        
        lua_newtable(L);
        if(query_start && *(query_start+1) != '\0'){
            char *query=strdup(query_start+1);
            while(query)
            {
                char *pair=strtok(query,"&");
                while(pair)
                {
                char *eq=strchr(pair,'=');
                if(eq){
                    *eq='\0';
                    const char *key=pair;
                    const char *val=eq+1;
                    lua_pushstring(L, val);
                    lua_setfield(L, -2, key);

                } pair = strtok(NULL, "&");
            } 
            free(query);
        }
            
        }
        lua_setfield(L, -2,"query");
        //request.headers = {}
        lua_newtable(L);
        char *line = strstr((char *)file_data, "\r\n");
if (line) {
    line += 2;
    while (line && *line && strncmp(line, "\r\n", 2) != 0) {
        char *colon = strchr(line, ':');
        if (!colon) break;
        *colon = '\0';
        char *key = line;
        char *val = colon + 1;
        while (*val == ' ') val++;
        char *newline = strstr(val, "\r\n");
        if (newline) *newline = '\0';
        lua_pushstring(L, val);
        lua_setfield(L, -2, key);
        if (newline) line = newline + 2;
        else break;
    }
}

        lua_setfield(L, -2, "headers");
        lua_setglobal(L,"request");

        int status=luaL_loadbuffer(L, (const char*)file_data,file_size,entry->filename);
        if(status==LUA_OK){
            status=lua_pcall(L,0,1,0);
        }
        if(status==LUA_OK && lua_isstring(L,-1)){
            const char *result = lua_tostring(L,-1);
            dprintf(client_fd, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n%s",
            strlen(result), result);
        }else{
            const char *err=lua_tostring(L,-1);
            dprintf(client_fd, "HTTP/1.1 500 Internal Server Error\r\n\r\nLua Error: %s", err? err:"unknown");
        }
        lua_close(L);
        return;
    }
    const char *mime_type = guess_content_type(entry->filename);
    char header[1024];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %lu\r\n"
             "Cache-Control: public, max-age=3600\r\n"
             "\r\n",
             mime_type, file_size);
    write(client_fd, header, strlen(header));
    write(client_fd, file_data, file_size);
}

int load_zip_from_self(const char *self_path) {
    FILE *fp = fopen(self_path, "rb");
    if (!fp) {
        fprintf(stderr, "Error: Could not open executable file\n"); return 0;
    }
    
    fseek(fp, 0, SEEK_END);
    long total_size = ftell(fp);
    if (total_size < 22) {
        fprintf(stderr, "Error: File too small to contain ZIP data\n");
        fclose(fp);
        return 0;
    }
    const int search_size = 65536;
    long find_start = (total_size > search_size) ? (total_size - search_size) : 0;
    size_t find_length = total_size - find_start;
    
    unsigned char *find_buffer = malloc(find_length);
    if (!find_buffer) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        fclose(fp);
        return 0;
    }
    
    fseek(fp, find_start, SEEK_SET);
    if (fread(find_buffer,1,find_length, fp)!=find_length) {
        fprintf(stderr, "Error: Could not read find buffer\n");
        free(find_buffer);
        fclose(fp);
        return 0;
    }
    long eocd_offset = -1;
    for (long i =find_length-22;i >= 0;i--) {
        if (memcmp(find_buffer + i, "PK\005\006", 4) == 0) {
            unsigned short comment_len = *(unsigned short *)(find_buffer + i + 20);
            if (find_start + i + 22 + comment_len==total_size) {
                eocd_offset = find_start +i;
                break;
            }
        }
    }
    free(find_buffer);
    if (eocd_offset == -1) {
        fprintf(stderr, "Error: No valid ZIP EOCD found\n");
        fclose(fp);
        return 0;
    }
    unsigned char eocd_data[22];
    fseek(fp, eocd_offset, SEEK_SET);
    if (fread(eocd_data,1,22, fp)!=22) {
        fprintf(stderr, "Error: Could not read EOCD\n");
        fclose(fp);
        return 0;
    }
    unsigned int cd_offset = *(unsigned int *)(eocd_data + 16);
    zip_start_offset = eocd_offset - (eocd_offset - cd_offset);
    for (long test_start = 0; test_start < eocd_offset; test_start++){
        fseek(fp, test_start, SEEK_SET);
        unsigned char sig[4];
        if (fread(sig, 4, 1, fp) == 1){
            if (memcmp(sig, "PK\003\004", 4) == 0){
                long test_cd_absolute = test_start + cd_offset;
                if (test_cd_absolute < eocd_offset){
                fseek(fp, test_cd_absolute, SEEK_SET);
                if (fread(sig, 4,1,fp)==1 && memcmp(sig, "PK\001\002",4) == 0){
                zip_start_offset = test_start;break;}
                }
            }
        }
    }
    
    if (zip_start_offset == -1) {
        fprintf(stderr,"Error: Could not determine ZIP start offset\n");
        fclose(fp);
        return 0;
    }
    zip_size = total_size - zip_start_offset;
    zip_data = malloc(zip_size);
    if (!zip_data){
        fprintf(stderr,"Error: Could not allocate memory for ZIP data\n");
        fclose(fp);
        return 0;
    }
    fseek(fp, zip_start_offset, SEEK_SET);
    if (fread(zip_data,1,zip_size,fp)!=zip_size) {
        fprintf(stderr,"Error: Could not read ZIP data\n");
        free(zip_data);
        zip_data = NULL;
        fclose(fp);return 0;
}
    fclose(fp); discover_zip_structure();return (zip_entry_cnt > 0);
}
char *zip_override_path = NULL;
int main(int argc, char **argv) {
    int port = PORT;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i], "--help")|| !strcmp(argv[i], "-h")){
            printf("Microbean - single-binary ZIP file server\n\n");
            printf("Usage: %s [options]\n", argv[0]);
            printf(" --help, -h       Show this help message\n");
            printf(" --port <number>  Use custom port instead of default (%d)\n",PORT);
            printf(" --dev            Enable dev mode \n");
            printf(" --fork           Enable fork() mode per request\n");
            printf(" --zip <file>     Use external zip file instead of embedded\n");
            printf(" --lua            Enable lua script execution for .lua files (sandboxed)\n");
            return 0;
        } else if(!strcmp(argv[i], "--port")) {
            if(i+1<argc) port=atoi(argv[++i]);
        else {fprintf(stderr, "Error: --port requires a number\n");return 1;} }
        else if (!strcmp(argv[i], "--dev")) dev_mode=1;
        else if (!strcmp(argv[i], "--lua")) use_lua=1;
        else if (!strcmp(argv[i], "--zip"))
        {
            if(i+1<argc)
            zip_override_path=argv[++i];
        else {
            fprintf(stderr, "Error: --zip requires a path to a .zip uncompressed file\n");
        }
        }
        else if(!strcmp(argv[i], "--fork")) use_fork=1;
        else
        {
            fprintf(stderr, "Unknown argument: %s\nuse --help to see available options", argv[i]); return 1;
        }
    }
    printf("Running Microbean Server\n");
    if (dev_mode)
    printf("Extracting embedded content...\n");
    if (zip_override_path){
        FILE *fp = fopen(zip_override_path, "rb");
        if(!fp){fprintf(stderr, "Error: Cannot open ZIP: %s\n", zip_override_path);
        return 1;}
        fseek(fp,0,SEEK_END);
        zip_size=ftell(fp);
        rewind(fp);
        zip_data=malloc(zip_size);
        if(!zip_data){
            fprintf(stderr, "Error: not enough memory to load ZIP\n"); 
            fclose(fp); 
            return 1;
        }
        fread(zip_data, 1, zip_size, fp);
        fclose(fp);
       if (dev_mode) fprintf(stderr, "Loaded external ZIP: %s (%zu bytes)\n",zip_override_path,zip_size);
       discover_zip_structure();
    } 
    else {
        if(dev_mode)
        fprintf(stderr, "No external ZIP provided. Falling back to embedded ZIP.\n");
        load_zip_from_self(argv[0]);
    }
    printf("successfully fetched %d files from embedded archive\n", zip_entry_cnt);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {.sin_family = AF_INET,.sin_addr.s_addr = INADDR_ANY,.sin_port = htons(port)};
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        return 1;
    }
    
    if (listen(fd, 10) < 0) {
        perror("listen failed");
        return 1;
    }
    printf("server running on http://localhost:%d\n", port);
    printf("available files:\n");
    for (int i =0; i<zip_entry_cnt;i++) {
        printf("  - %s\n", zip_contents[i].filename);
    } printf("\npress ctrl+c to stop\n\n");
    fd_set readfds;
    int maxfd=fd;
    signal(SIGCHLD, SIG_IGN); // auto reap child
    while(1){
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        int ready=select(fd+1, &readfds, NULL, NULL, NULL);
        if (ready<0)
        {
            if(dev_mode) perror("select()");
            continue;
        }
        if(FD_ISSET(fd, &readfds)){
            int client_fd=accept(fd, NULL, NULL);
            if(client_fd<0) continue;
            if(use_fork){
                pid_t pid = fork();
                if (pid==0){
                    // --- child process ---
                    close(fd);
                    char request[MAX_REQ]={0};
                    ssize_t bytes_read = read(client_fd, request, sizeof(request)-1);
                    if(bytes_read>0){
                        char method[16], path[MAX_PATH], version[16];
                        if(sscanf(request, "%15s %1023s %15s", method, path, version)==3){
                            if(dev_mode) printf("request: %s %s\n", method, path);
                            if(strcmp(method, "GET")==0 || strcmp(method, "HEAD")==0) serve_path(client_fd, path);
                            else {const char *response = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
                            write(client_fd, response, strlen(response));}
                        }
                    }
                    close(client_fd); // close child
                 _exit(0);
                } else if (pid>0) {
                    // --- parent process ---
                    close(client_fd);
                
                }
                else { perror("fork falied");
                close(client_fd);}
            }
            else
            {
                char request[MAX_REQ]={0};
                ssize_t bytes_read=read(client_fd, request, sizeof(request)-1);
                if (bytes_read>0){
                    char method[16], path[MAX_PATH], version[16];
                    if(sscanf(request, "%15s %1023s %15s", method, path, version)==3){
                        if(dev_mode) printf("request: %s %s\n", method, path);
                        if(strcmp(method, "GET")==0 || strcmp(method, "HEAD")==0){
                            serve_path(client_fd, path);

                        }else{
                            const char *response="HTTP/1.1 405 Method Not Allowed\r\n\r\n";
                            write(client_fd, response, strlen(response));
                        }
                    }
                }
                close(client_fd);
            }
        }
        
    }
    if (zip_data) {
        free(zip_data);
    }
    return 0;
}
