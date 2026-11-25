FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev libpq-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY main.c .
RUN gcc -O2 -Wall -Wextra -o nexora-api main.c $(pkg-config --cflags --libs libpq)
CMD ["./nexora-api"]
