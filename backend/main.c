#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#define PORT_DEFAULT 8080
#define REQ_SIZE 32768

static PGconn *db = NULL;

static void send_response(int client, int status, const char *status_text, const char *type, const char *body) {
    char header[1024];
    size_t len = strlen(body);
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\n\r\n", status, status_text, type, len);
    if (n > 0) send(client, header, (size_t)n, 0);
    send(client, body, len, 0);
}

static void json_error(int c, int status, const char *msg) {
    char body[512];
    snprintf(body, sizeof(body), "{\"success\":false,\"error\":\"%s\"}", msg);
    const char *text = status == 404 ? "Not Found" : status == 409 ? "Conflict" : status >= 500 ? "Internal Server Error" : "Bad Request";
    send_response(c, status, text, "application/json; charset=utf-8", body);
}

static void json_escape(const char *src, char *dst, size_t cap) {
    size_t j = 0;
    if (!src || cap == 0) return;
    for (size_t i = 0; src[i] && j + 2 < cap; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '"' || ch == '\\') { dst[j++] = '\\'; dst[j++] = (char)ch; }
        else if (ch == '\n' || ch == '\r') dst[j++] = ' ';
        else if (ch >= 32) dst[j++] = (char)ch;
    }
    dst[j] = '\0';
}

static int get_json_string(const char *json, const char *key, char *out, size_t cap) {
    char needle[100];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return 0;
    p++;
    size_t j = 0;
    while (*p && *p != '"' && j + 1 < cap) {
        if (*p == '\\' && p[1]) p++;
        out[j++] = *p++;
    }
    out[j] = '\0';
    return *p == '"';
}

static int valid_date(const char *s, time_t *out) {
    if (strlen(s) != 10 || s[4] != '-' || s[7] != '-') return 0;
    struct tm tmv = {0};
    char *end = strptime(s, "%Y-%m-%d", &tmv);
    if (!end || *end) return 0;
    tmv.tm_isdst = 0;
    time_t t = timegm(&tmv);
    struct tm check;
    gmtime_r(&t, &check);
    if (check.tm_year != tmv.tm_year || check.tm_mon != tmv.tm_mon || check.tm_mday != tmv.tm_mday) return 0;
    *out = t;
    return 1;
}

static int room_info(const char *key, const char **name, int *price, int *capacity) {
    if (!strcmp(key, "standard")) { *name = "Standard Room"; *price = 3500; *capacity = 10; return 1; }
    if (!strcmp(key, "deluxe")) { *name = "Deluxe Room"; *price = 5500; *capacity = 6; return 1; }
    if (!strcmp(key, "executive")) { *name = "Executive Suite"; *price = 8500; *capacity = 3; return 1; }
    return 0;
}

static int init_db(const char *conninfo) {
    db = PQconnectdb(conninfo);
    if (PQstatus(db) != CONNECTION_OK) {
        fprintf(stderr, "PostgreSQL connection failed: %s\n", PQerrorMessage(db));
        PQfinish(db);
        db = NULL;
        return 0;
    }
    const char *sql =
        "CREATE TABLE IF NOT EXISTS bookings ("
        "id BIGSERIAL PRIMARY KEY, booking_code TEXT UNIQUE, guest_name TEXT NOT NULL,"
        "email TEXT NOT NULL, phone TEXT NOT NULL, room_type TEXT NOT NULL,"
        "check_in DATE NOT NULL, check_out DATE NOT NULL, nights INTEGER NOT NULL,"
        "total_amount INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'Confirmed',"
        "created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP)";
    PGresult *res = PQexec(db, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Database initialization failed: %s\n", PQerrorMessage(db));
        PQclear(res);
        return 0;
    }
    PQclear(res);
    return 1;
}

static void handle_request(int c, char *req) {
    char method[12] = {0}, path[1024] = {0};
    sscanf(req, "%11s %1023s", method, path);

    if (!strcmp(method, "OPTIONS")) { send_response(c, 204, "No Content", "text/plain", ""); return; }
    if (!strcmp(method, "GET") && !strcmp(path, "/health")) {
        send_response(c, 200, "OK", "application/json", "{\"success\":true,\"service\":\"Nexora C booking API\",\"database\":\"PostgreSQL\"}");
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/rooms")) {
        const char *body = "{\"success\":true,\"rooms\":[{\"id\":\"standard\",\"name\":\"Standard Room\",\"price\":3500,\"capacity\":10},{\"id\":\"deluxe\",\"name\":\"Deluxe Room\",\"price\":5500,\"capacity\":6},{\"id\":\"executive\",\"name\":\"Executive Suite\",\"price\":8500,\"capacity\":3}]}";
        send_response(c, 200, "OK", "application/json", body);
        return;
    }
    if (!strcmp(method, "GET") && !strncmp(path, "/api/bookings?code=", 19)) {
        char code[80] = {0};
        snprintf(code, sizeof(code), "%s", path + 19);
        for (size_t i = 0; code[i]; i++) if (code[i] == '+') code[i] = ' ';
        const char *values[] = { code };
        PGresult *res = PQexecParams(db,
            "SELECT booking_code,guest_name,room_type,check_in::text,check_out::text,nights,total_amount,status FROM bookings WHERE booking_code=$1",
            1, NULL, values, NULL, NULL, 0);
        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
            char guest[256], room[80], ci[20], co[20], status[40], bc[80];
            json_escape(PQgetvalue(res, 0, 1), guest, sizeof guest);
            const char *rn = "Room"; int pr = 0, cap = 0;
            room_info(PQgetvalue(res, 0, 2), &rn, &pr, &cap);
            snprintf(room, sizeof room, "%s", rn);
            snprintf(ci, sizeof ci, "%s", PQgetvalue(res, 0, 3));
            snprintf(co, sizeof co, "%s", PQgetvalue(res, 0, 4));
            snprintf(status, sizeof status, "%s", PQgetvalue(res, 0, 7));
            snprintf(bc, sizeof bc, "%s", PQgetvalue(res, 0, 0));
            char body[1200];
            snprintf(body, sizeof body,
                "{\"success\":true,\"booking\":{\"bookingCode\":\"%s\",\"guestName\":\"%s\",\"roomName\":\"%s\",\"checkIn\":\"%s\",\"checkOut\":\"%s\",\"nights\":%s,\"totalAmount\":%s,\"status\":\"%s\"}}",
                bc, guest, room, ci, co, PQgetvalue(res, 0, 5), PQgetvalue(res, 0, 6), status);
            send_response(c, 200, "OK", "application/json", body);
        } else if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            json_error(c, 404, "No reservation was found for that booking reference.");
        } else {
            fprintf(stderr, "Booking lookup failed: %s\n", PQerrorMessage(db));
            json_error(c, 500, "Could not look up reservation. Please try again.");
        }
        PQclear(res);
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/bookings")) {
        char name[120] = {0}, email[160] = {0}, phone[40] = {0}, room[30] = {0}, ci[20] = {0}, co[20] = {0};
        if (!get_json_string(req, "guestName", name, sizeof name) || !get_json_string(req, "email", email, sizeof email) ||
            !get_json_string(req, "phone", phone, sizeof phone) || !get_json_string(req, "roomType", room, sizeof room) ||
            !get_json_string(req, "checkIn", ci, sizeof ci) || !get_json_string(req, "checkOut", co, sizeof co)) {
            json_error(c, 400, "Please complete all booking fields."); return;
        }
        if (strlen(name) < 2 || !strchr(email, '@') || strlen(phone) < 7) { json_error(c, 400, "Please provide valid guest contact details."); return; }
        const char *room_name; int price, capacity;
        if (!room_info(room, &room_name, &price, &capacity)) { json_error(c, 400, "Please select a valid room type."); return; }
        time_t tin, tout;
        if (!valid_date(ci, &tin) || !valid_date(co, &tout) || tout <= tin) { json_error(c, 400, "Please enter valid dates with check-out after check-in."); return; }
        time_t now = time(NULL); struct tm today_tm; gmtime_r(&now, &today_tm);
        today_tm.tm_hour = 0; today_tm.tm_min = 0; today_tm.tm_sec = 0;
        if (tin < timegm(&today_tm)) { json_error(c, 400, "Check-in date cannot be in the past."); return; }
        int nights = (int)((tout - tin) / 86400);
        if (nights < 1 || nights > 90) { json_error(c, 400, "A stay must be between 1 and 90 nights."); return; }

        PGresult *begin = PQexec(db, "BEGIN");
        if (PQresultStatus(begin) != PGRES_COMMAND_OK) { PQclear(begin); json_error(c, 500, "Could not start reservation. Please try again."); return; }
        PQclear(begin);
        char nights_s[16]; snprintf(nights_s, sizeof nights_s, "%d", nights);
        const char *count_values[] = { room, co, ci };
        PGresult *count_res = PQexecParams(db,
            "SELECT COUNT(*) FROM bookings WHERE room_type=$1 AND status='Confirmed' AND check_in < $2::date AND check_out > $3::date",
            3, NULL, count_values, NULL, NULL, 0);
        if (PQresultStatus(count_res) != PGRES_TUPLES_OK) {
            PQclear(count_res); PQexec(db, "ROLLBACK"); json_error(c, 500, "Could not check room availability."); return;
        }
        int occupied = atoi(PQgetvalue(count_res, 0, 0));
        PQclear(count_res);
        if (occupied >= capacity) { PQexec(db, "ROLLBACK"); json_error(c, 409, "Sorry, this room type is fully booked for the selected dates. Please try other dates."); return; }

        char total_s[32]; snprintf(total_s, sizeof total_s, "%d", nights * price);
        PGresult *ins = PQexecParams(db,
            "INSERT INTO bookings(guest_name,email,phone,room_type,check_in,check_out,nights,total_amount) VALUES($1,$2,$3,$4,$5::date,$6::date,$7::integer,$8::integer) RETURNING id",
            8, NULL, (const char *const[]){ name, email, phone, room, ci, co, nights_s, total_s }, NULL, NULL, 0);
        if (PQresultStatus(ins) != PGRES_TUPLES_OK) {
            fprintf(stderr, "Booking insert failed: %s\n", PQerrorMessage(db));
            PQclear(ins); PQexec(db, "ROLLBACK"); json_error(c, 500, "Could not save reservation. Please try again."); return;
        }
        long long id = atoll(PQgetvalue(ins, 0, 0));
        PQclear(ins);
        char code[40]; snprintf(code, sizeof code, "NX%06lld", id);
        const char *update_values[] = { code, "" };
        char id_s[32]; snprintf(id_s, sizeof id_s, "%lld", id);
        update_values[1] = id_s;
        PGresult *upd = PQexecParams(db, "UPDATE bookings SET booking_code=$1 WHERE id=$2::bigint", 2, NULL, update_values, NULL, NULL, 0);
        if (PQresultStatus(upd) != PGRES_COMMAND_OK) {
            PQclear(upd); PQexec(db, "ROLLBACK"); json_error(c, 500, "Could not finish reservation. Please try again."); return;
        }
        PQclear(upd);
        PGresult *commit = PQexec(db, "COMMIT");
        if (PQresultStatus(commit) != PGRES_COMMAND_OK) {
            PQclear(commit); PQexec(db, "ROLLBACK"); json_error(c, 500, "Could not confirm reservation. Please try again."); return;
        }
        PQclear(commit);
        char safe_name[256], safe_room[100];
        json_escape(name, safe_name, sizeof safe_name); json_escape(room_name, safe_room, sizeof safe_room);
        char body[700];
        snprintf(body, sizeof body, "{\"success\":true,\"bookingCode\":\"%s\",\"guestName\":\"%s\",\"roomName\":\"%s\",\"nights\":%d,\"totalAmount\":%d}", code, safe_name, safe_room, nights, nights * price);
        send_response(c, 201, "Created", "application/json", body);
        return;
    }
    json_error(c, 404, "Endpoint not found.");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    const char *conninfo = getenv("DATABASE_URL");
    if (!conninfo || !*conninfo) {
        fprintf(stderr, "DATABASE_URL environment variable is missing.\n");
        return 1;
    }
    if (!init_db(conninfo)) return 1;
    int port = PORT_DEFAULT; const char *p = getenv("PORT"); if (p) port = atoi(p);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { perror("socket"); PQfinish(db); return 1; }
    int yes = 1; setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr = {0}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons((unsigned short)port);
    if (bind(server, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(server, 32) < 0) { perror("bind/listen"); close(server); PQfinish(db); return 1; }
    printf("Nexora C booking API (PostgreSQL) listening on port %d\n", port); fflush(stdout);
    while (1) {
        int client = accept(server, NULL, NULL);
        if (client < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        char req[REQ_SIZE]; size_t used = 0; ssize_t n; int header_end = 0; long content_length = 0;
        while (used < sizeof(req) - 1) {
            n = recv(client, req + used, sizeof(req) - 1 - used, 0);
            if (n <= 0) break;
            used += (size_t)n; req[used] = '\0';
            char *end = strstr(req, "\r\n\r\n");
            if (end && !header_end) { header_end = (int)(end + 4 - req); char *cl = strcasestr(req, "Content-Length:"); if (cl) content_length = strtol(cl + 15, NULL, 10); }
            if (header_end && used >= (size_t)header_end + (size_t)content_length) break;
        }
        if (used > 0) handle_request(client, req);
        close(client);
    }
    close(server);
    PQfinish(db);
    return 0;
}
