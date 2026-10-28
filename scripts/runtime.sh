# Sourced by port.sh after cxx.sh: shared_runtime sets runtime_lib to a current libps2runtime.a, built once in
# build/runtime-native-<key> instead of once per game. There is one folder per compiler, flags and build type,
# which ninja keeps up to date, so a changed runtime source rebuilds just its object. A lock folder keeps port.sh
# runs at the same time from building together: the others wait, then find the library current.
# PS2_SHARED_RUNTIME=0 leaves runtime_lib empty and every game compiles its own copy.
build_type=Release
runtime_lib=
runtime_ready=0

runtime_lock() {
  local lock="$1" owner waited=0
  until mkdir "$lock" 2> /dev/null; do
    owner=$(cat "$lock/pid" 2> /dev/null || true)
    if { [ -n "$owner" ] && ! kill -0 "$owner" 2> /dev/null; } || [ -n "$(find "$lock" -maxdepth 0 -mmin +60 2> /dev/null)" ] \
        || { [ -z "$owner" ] && [ -n "$(find "$lock" -maxdepth 0 -mmin +1 2> /dev/null)" ]; }; then
      echo "runtime: the port.sh that held ${lock#$ROOT/} is gone, taking over its lock"
      rm -rf "$lock"
      continue
    fi
    [ "$waited" = 1 ] || echo "runtime: another port.sh is building it, waiting"
    waited=1
    sleep 1
  done
  echo $$ > "$lock/pid"
}

# Configures the folder if needed and builds the library; the lock is held.
runtime_build() {
  local dir="$1"
  if [ ! -f "$dir/build.ninja" ]; then
    mkdir -p "$dir"
    cmake -S "$ROOT" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE="$build_type" "${cxx_flags[@]}" > "$dir/cmake.log" 2>&1 || { tail -30 "$dir/cmake.log"; return 1; }
  fi
  ninja -C "$dir" ${ninja_jobs:-} ps2runtime > "$dir/ninja.log" 2>&1 || { tail -30 "$dir/ninja.log"; return 1; }
  local built
  built=$(grep -c ' Building ' "$dir/ninja.log" || true)
  [ "$built" = 0 ] || echo "runtime: built $built files in ${dir#$ROOT/}"
}

shared_runtime() {
  runtime_lib=
  [ "${PS2_SHARED_RUNTIME:-1}" != 0 ] || return 0
  local gpu=0 key dir
  [ ! -f "$ROOT/third_party/wgpu-native/lib/libwgpu_native.a" ] || gpu=1
  key=$(printf '%s' "native $build_type gpu$gpu ${CXXFLAGS:-} ${cxx_stamp:-} ${cxx_flags[*]}" | cksum | cut -d' ' -f1)
  dir="$ROOT/build/runtime-native-$key"
  if [ "$runtime_ready" != 1 ]; then
    mkdir -p "$ROOT/build"
    runtime_lock "$dir.lock"
    runtime_lock_dir="$dir.lock"
    trap 'rm -rf "$runtime_lock_dir"' EXIT
    trap 'exit 130' INT TERM
    local status=0
    runtime_build "$dir" || status=$?
    rm -rf "$dir.lock"
    trap - EXIT INT TERM
    [ "$status" = 0 ] || exit "$status"
    runtime_ready=1
  fi
  runtime_lib="$dir/libps2runtime.a"
}
