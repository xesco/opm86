# Ubuntu with what building OPM86 Player needs, for make linux.
FROM ubuntu:24.04
COPY scripts/linux-deps.sh /tmp/linux-deps.sh
RUN /tmp/linux-deps.sh && rm -rf /var/lib/apt/lists/*
