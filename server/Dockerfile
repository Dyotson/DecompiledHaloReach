# Reach Live server: docker build -t reach-live server/ &&
#   docker run -p 21100:21100/udp -p 21101:21101 -v reach-live-data:/data reach-live
FROM python:3.12-slim
WORKDIR /app
COPY *.py ./
EXPOSE 21100/udp 21101/tcp
VOLUME /data
CMD ["python3", "reach_live_server.py", "--port", "21100", "--http-port", "21101", "--data-dir", "/data"]
