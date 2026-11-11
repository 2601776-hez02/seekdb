# MiniVectorDB — 评测容器镜像
# 构建: docker build -t minivectordb:latest .
# 运行: docker run --rm -i -v <data-volume>:/data minivectordb:latest
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY mvdb/ /app/

RUN make -C /app clean && make -C /app -j"$(nproc)" \
    CXXFLAGS="-O2 -std=c++17 -pthread -DNDEBUG -DMVDB_WANT_AVX2 -mavx2 -mfma"

RUN mkdir -p /data
VOLUME ["/data"]

ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]
