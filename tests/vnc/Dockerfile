# Minimal VNC target for exercising the Guacamole gateway end-to-end.
#
# Runs a headless X server (Xvfb) + a lightweight window manager (fluxbox)
# exposed over RFB by x11vnc. It exists only to give the libguac VNC client
# a real server to connect to during tests — it is NOT a production image.
FROM alpine:3.19

# x11vnc / xvfb / fluxbox / xterm live in the community repository.
RUN echo "https://dl-cdn.alpinelinux.org/alpine/v3.19/community" >> /etc/apk/repositories \
    && apk add --no-cache x11vnc xvfb fluxbox xterm bash

ENV DISPLAY=:0 \
    VNC_PORT=5900 \
    GEOMETRY=1024x768x24

# Password is provided at run time via VNC_PASSWORD (default below).
ENV VNC_PASSWORD=guacvnc

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

EXPOSE 5900
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
