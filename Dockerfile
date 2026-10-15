FROM gcc:11.3 AS build

RUN apt update && \
    apt install -y cmake python3-pip wget && \
    pip3 install conan==1.53.0

WORKDIR /app

COPY conanfile.txt .
RUN pip install --upgrade "conan>=1.54.0,<2.0"
RUN mkdir build && cd build && \
    conan install .. --build=missing -s compiler.libcxx=libstdc++11 -s build_type=Release

COPY CMakeLists.txt .
COPY src ./src

RUN wget -q -O /app/src/httplib.h https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.14.3/httplib.h && \
    wget -q -O /app/src/json.hpp https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp

RUN cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    cmake --build .

FROM gcc:11.3

RUN apt update && \
    apt install -y libpq5 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=build /app/build/game_server /app/game_server
COPY game_server.sh /app/game_server.sh
COPY data /app/data
COPY static /app/static

RUN chmod +x /app/game_server /app/game_server.sh

EXPOSE 8080
ENTRYPOINT ["/app/game_server.sh"]
CMD ["--config-file", "./data/config.json", "--www-root", "./static"]
