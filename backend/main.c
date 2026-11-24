#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#define PORT_DEFAULT 8080
#define REQ_SIZE 32768
#define DB_DEFAULT "/var/data/nexora.db"

static sqlite3 *db;

static void send_response(int client, int status, const char *status_text, const char *type, const char *body) {
    char header[1024];
    size_t len = strlen(body);
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\n\r\n", status, status_text, type, len);
    send(client, header, (size_t)n, 0);
    send(client, body, len, 0);
}
static void json_error(int c, int status, const char *msg) {
    char body[512];
    snprintf(body, sizeof(body), "{\"success\":false,\"error\":\"%s\"}", msg);
    send_response(c, status, status == 404 ? "Not Found" : status == 409 ? "Conflict" : "Bad Request", "application/json; charset=utf-8", body);
}
static void json_escape(const char *src, char *dst, size_t cap) {
    size_t j=0;
    for (size_t i=0; src[i] && j+2<cap; i++) {
        unsigned char c=(unsigned char)src[i];
        if (c=='"' || c=='\\') { dst[j++]='\\'; dst[j++]=(char)c; }
        else if (c=='\n' || c=='\r') { dst[j++]=' '; }
        else if (c>=32) dst[j++]=(char)c;
    }
    dst[j]='\0';
}
static int get_json_string(const char *json, const char *key, char *out, size_t cap) {
    char needle[100]; snprintf(needle,sizeof(needle),"\"%s\"",key);
    const char *p=strstr(json,needle); if(!p) return 0;
    p=strchr(p,':'); if(!p) return 0; p++;
    while(*p && isspace((unsigned char)*p)) p++;
    if(*p!='"') return 0; p++;
    size_t j=0;
    while(*p && *p!='"' && j+1<cap) {
        if(*p=='\\' && p[1]) p++;
        out[j++]=*p++;
    }
    out[j]='\0'; return *p=='"';
}
static int valid_date(const char *s, time_t *out) {
    if(strlen(s)!=10 || s[4]!='-' || s[7]!='-') return 0;
    struct tm tm={0}; char *end=strptime(s,"%Y-%m-%d",&tm);
    if(!end || *end) return 0;
    tm.tm_isdst=0; time_t t=timegm(&tm);
    struct tm check; gmtime_r(&t,&check);
    if(check.tm_year!=tm.tm_year || check.tm_mon!=tm.tm_mon || check.tm_mday!=tm.tm_mday) return 0;
    *out=t; return 1;
}
static int room_info(const char *key, const char **name, int *price, int *capacity) {
    if(!strcmp(key,"standard")) {*name="Standard Room";*price=3500;*capacity=10;return 1;}
    if(!strcmp(key,"deluxe")) {*name="Deluxe Room";*price=5500;*capacity=6;return 1;}
    if(!strcmp(key,"executive")) {*name="Executive Suite";*price=8500;*capacity=3;return 1;}
    return 0;
}
static int init_db(const char *path) {
    if(sqlite3_open(path,&db)!=SQLITE_OK) {fprintf(stderr,"Database open failed: %s\n",sqlite3_errmsg(db));return 0;}
    const char *sql =
      "CREATE TABLE IF NOT EXISTS bookings ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, booking_code TEXT UNIQUE, guest_name TEXT NOT NULL,"
      "email TEXT NOT NULL, phone TEXT NOT NULL, room_type TEXT NOT NULL,"
      "check_in TEXT NOT NULL, check_out TEXT NOT NULL, nights INTEGER NOT NULL,"
      "total_amount INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'Confirmed', created_at TEXT DEFAULT CURRENT_TIMESTAMP);";
    char *err=NULL; int rc=sqlite3_exec(db,sql,NULL,NULL,&err);
    if(rc!=SQLITE_OK){fprintf(stderr,"Database init: %s\n",err?err:"unknown");sqlite3_free(err);return 0;}
    return 1;
}
static void handle_request(int c, char *req) {
    char method[12]={0}, path[1024]={0};
    sscanf(req,"%11s %1023s",method,path);
    if(!strcmp(method,"OPTIONS")) { send_response(c,204,"No Content","text/plain",""); return; }
    if(!strcmp(method,"GET") && !strcmp(path,"/health")) {send_response(c,200,"OK","application/json","{\"success\":true,\"service\":\"Nexora C booking API\"}");return;}
    if(!strcmp(method,"GET") && !strcmp(path,"/api/rooms")) {
        const char *body="{\"success\":true,\"rooms\":[{\"id\":\"standard\",\"name\":\"Standard Room\",\"price\":3500,\"capacity\":10},{\"id\":\"deluxe\",\"name\":\"Deluxe Room\",\"price\":5500,\"capacity\":6},{\"id\":\"executive\",\"name\":\"Executive Suite\",\"price\":8500,\"capacity\":3}]}";
        send_response(c,200,"OK","application/json",body);return;
    }
    if(!strcmp(method,"GET") && !strncmp(path,"/api/bookings?code=",19)) {
        char code[80]={0}; snprintf(code,sizeof(code),"%s",path+19);
        for(size_t i=0;code[i];i++) if(code[i]=='%') code[i]=' '; 
        sqlite3_stmt *st=NULL;
        const char *sql="SELECT booking_code,guest_name,room_type,check_in,check_out,nights,total_amount,status FROM bookings WHERE booking_code=?";
        sqlite3_prepare_v2(db,sql,-1,&st,NULL); sqlite3_bind_text(st,1,code,-1,SQLITE_TRANSIENT);
        if(sqlite3_step(st)==SQLITE_ROW) {
            char guest[256],room[80],ci[20],co[20],status[40],bc[80]; json_escape((const char*)sqlite3_column_text(st,1),guest,sizeof guest);
            const char *rn;int pr,cap; room_info((const char*)sqlite3_column_text(st,2),&rn,&pr,&cap);
            snprintf(room,sizeof room,"%s",rn); snprintf(ci,sizeof ci,"%s",sqlite3_column_text(st,3)); snprintf(co,sizeof co,"%s",sqlite3_column_text(st,4));
            snprintf(status,sizeof status,"%s",sqlite3_column_text(st,7)); snprintf(bc,sizeof bc,"%s",sqlite3_column_text(st,0));
            char body[1200]; snprintf(body,sizeof body,"{\"success\":true,\"booking\":{\"bookingCode\":\"%s\",\"guestName\":\"%s\",\"roomName\":\"%s\",\"checkIn\":\"%s\",\"checkOut\":\"%s\",\"nights\":%d,\"totalAmount\":%d,\"status\":\"%s\"}}",bc,guest,room,ci,co,sqlite3_column_int(st,5),sqlite3_column_int(st,6),status);
            send_response(c,200,"OK","application/json",body);
        } else json_error(c,404,"No reservation was found for that booking reference.");
        sqlite3_finalize(st);return;
    }
    if(!strcmp(method,"POST") && !strcmp(path,"/api/bookings")) {
        char name[120]={0},email[160]={0},phone[40]={0},room[30]={0},ci[20]={0},co[20]={0};
        if(!get_json_string(req,"guestName",name,sizeof name)||!get_json_string(req,"email",email,sizeof email)||
           !get_json_string(req,"phone",phone,sizeof phone)||!get_json_string(req,"roomType",room,sizeof room)||
           !get_json_string(req,"checkIn",ci,sizeof ci)||!get_json_string(req,"checkOut",co,sizeof co)) {json_error(c,400,"Please complete all booking fields.");return;}
        if(strlen(name)<2||!strchr(email,'@')||strlen(phone)<7){json_error(c,400,"Please provide valid guest contact details.");return;}
        const char *room_name;int price,capacity;
        if(!room_info(room,&room_name,&price,&capacity)){json_error(c,400,"Please select a valid room type.");return;}
        time_t tin,tout;
        if(!valid_date(ci,&tin)||!valid_date(co,&tout)||tout<=tin){json_error(c,400,"Please enter valid dates with check-out after check-in.");return;}
        time_t now=time(NULL); struct tm today_tm; gmtime_r(&now,&today_tm); today_tm.tm_hour=0;today_tm.tm_min=0;today_tm.tm_sec=0;
        if(tin < timegm(&today_tm)){json_error(c,400,"Check-in date cannot be in the past.");return;}
        int nights=(int)((tout-tin)/86400); if(nights<1||nights>90){json_error(c,400,"A stay must be between 1 and 90 nights.");return;}
        sqlite3_stmt *st=NULL;
        const char *count_sql="SELECT COUNT(*) FROM bookings WHERE room_type=? AND status='Confirmed' AND check_in < ? AND check_out > ?";
        sqlite3_prepare_v2(db,count_sql,-1,&st,NULL);sqlite3_bind_text(st,1,room,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,2,co,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,ci,-1,SQLITE_TRANSIENT);
        int occupied=0;if(sqlite3_step(st)==SQLITE_ROW)occupied=sqlite3_column_int(st,0);sqlite3_finalize(st);
        if(occupied>=capacity){json_error(c,409,"Sorry, this room type is fully booked for the selected dates. Please try other dates.");return;}
        const char *ins="INSERT INTO bookings(guest_name,email,phone,room_type,check_in,check_out,nights,total_amount) VALUES(?,?,?,?,?,?,?,?)";
        sqlite3_prepare_v2(db,ins,-1,&st,NULL);
        sqlite3_bind_text(st,1,name,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,2,email,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,phone,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,4,room,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,5,ci,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,6,co,-1,SQLITE_TRANSIENT);sqlite3_bind_int(st,7,nights);sqlite3_bind_int(st,8,nights*price);
        if(sqlite3_step(st)!=SQLITE_DONE){sqlite3_finalize(st);json_error(c,500,"Could not save reservation. Please try again.");return;}
        sqlite3_finalize(st); long id=(long)sqlite3_last_insert_rowid(db); char code[40];snprintf(code,sizeof code,"NX%06ld",id);
        sqlite3_prepare_v2(db,"UPDATE bookings SET booking_code=? WHERE id=?",-1,&st,NULL);sqlite3_bind_text(st,1,code,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(st,2,id);sqlite3_step(st);sqlite3_finalize(st);
        char safe_name[256],safe_room[100];json_escape(name,safe_name,sizeof safe_name);json_escape(room_name,safe_room,sizeof safe_room);
        char body[700];snprintf(body,sizeof body,"{\"success\":true,\"bookingCode\":\"%s\",\"guestName\":\"%s\",\"roomName\":\"%s\",\"nights\":%d,\"totalAmount\":%d}",code,safe_name,safe_room,nights,nights*price);
        send_response(c,201,"Created","application/json",body);return;
    }
    json_error(c,404,"Endpoint not found.");
}
int main(void) {
    signal(SIGPIPE,SIG_IGN);
    const char *dbpath=getenv("DB_PATH");if(!dbpath)dbpath=DB_DEFAULT;
    if(!init_db(dbpath))return 1;
    int port=PORT_DEFAULT;const char *p=getenv("PORT");if(p)port=atoi(p);
    int server=socket(AF_INET,SOCK_STREAM,0);if(server<0){perror("socket");return 1;}
    int yes=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_addr.s_addr=INADDR_ANY;addr.sin_port=htons((unsigned short)port);
    if(bind(server,(struct sockaddr*)&addr,sizeof addr)<0||listen(server,32)<0){perror("bind/listen");return 1;}
    printf("Nexora C booking API listening on port %d\n",port);fflush(stdout);
    while(1){
        int client=accept(server,NULL,NULL);if(client<0){if(errno==EINTR)continue;perror("accept");continue;}
        char req[REQ_SIZE];size_t used=0;ssize_t n;int header_end=0;long content_length=0;
        while(used<sizeof(req)-1){
            n=recv(client,req+used,sizeof(req)-1-used,0);if(n<=0)break;used+=(size_t)n;req[used]='\0';
            char *end=strstr(req,"\r\n\r\n");
            if(end&&!header_end){header_end=(int)(end+4-req);char *cl=strcasestr(req,"Content-Length:");if(cl)content_length=strtol(cl+15,NULL,10);}
            if(header_end && used >= (size_t)header_end+(size_t)content_length)break;
        }
        if(used>0)handle_request(client,req);close(client);
    }
    sqlite3_close(db);return 0;
}
