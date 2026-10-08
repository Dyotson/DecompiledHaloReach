#!/usr/bin/env bash
set -euo pipefail

GHIDRA_HOME="${GHIDRA_HOME:-$HOME/ghidra_12.1.4_PUBLIC}"
MCP_JAR="${MCP_JAR:-$HOME/ghidra-mcp/build/libs/GhidraMCP-7.0.0.jar}"
USER_EXT="$HOME/.config/ghidra/ghidra_12.1.4_PUBLIC/Extensions"
export JAVA_HOME="${JAVA_HOME:-/home/linuxbrew/.linuxbrew/opt/openjdk@21/libexec}"

CP="$MCP_JAR"
for jar in "$GHIDRA_HOME"/Ghidra/{Framework,Features,Processors}/*/lib/*.jar "$USER_EXT"/XEXLoaderWV/lib/*.jar; do
    CP="$CP:$jar"
done

exec "$JAVA_HOME/bin/java" -Xmx16g -XX:+UseG1GC \
    -Dghidra.install.dir="$GHIDRA_HOME" \
    -cp "$CP" com.xebyte.headless.GhidraMCPHeadlessServer \
    --bind 127.0.0.1 --port "${GHIDRA_MCP_PORT:-8089}" "$@"
