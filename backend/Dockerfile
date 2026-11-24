FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev libsqlite3-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY main.c .
RUN gcc -O2 -Wall -Wextra -o nexora-api main.c -lsqlite3
ENV PORT=8080
ENV DB_PATH=/var/data/nexora.db
EXPOSE 8080
CMD ["./nexora-api"]
