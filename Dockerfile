# Multi-stage build.
#
# The build stage carries a compiler, CMake and make; the final stage carries nothing at
# all. Linking statically against musl makes that possible: the runtime image is the five
# executables and nothing else, so there is no package manager, no shell and no library to
# keep patched. The cost is that the image has no tools for poking at a running container,
# which is a fair trade for a workload whose only interface is three HTTP endpoints.
#
# The unit tests run inside the build stage. An image that compiled but fails its own
# tests should not exist.

FROM alpine:3.20 AS build
# build-base brings gcc, g++, musl-dev and make. Make rather than Ninja on purpose: it is
# already in build-base, and one fewer package is one fewer thing that can move.
RUN apk add --no-cache build-base cmake
WORKDIR /src

COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests

RUN cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_EXE_LINKER_FLAGS="-static" \
 && cmake --build build -j "$(nproc)" \
 && ctest --test-dir build --output-on-failure \
 && strip build/stratus-worker build/stratus-proxy build/stratus-loadgen \
          build/stratus-autoscaler build/stratus-cost

FROM scratch
COPY --from=build /src/build/stratus-worker     /stratus-worker
COPY --from=build /src/build/stratus-proxy      /stratus-proxy
COPY --from=build /src/build/stratus-loadgen    /stratus-loadgen
COPY --from=build /src/build/stratus-autoscaler /stratus-autoscaler
COPY --from=build /src/build/stratus-cost       /stratus-cost

EXPOSE 8080 8081

# CMD, not ENTRYPOINT. One image carries all five executables and the compose file selects
# one per service with `command:`; an ENTRYPOINT would turn that selection into arguments
# passed to the worker, and every service would silently start as a worker.
CMD ["/stratus-worker"]
