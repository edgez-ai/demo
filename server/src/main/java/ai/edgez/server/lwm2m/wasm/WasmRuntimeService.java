package ai.edgez.server.lwm2m.wasm;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;
import java.util.Set;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import io.github.kawamuray.wasmtime.Engine;
import io.github.kawamuray.wasmtime.Extern;
import io.github.kawamuray.wasmtime.Instance;
import io.github.kawamuray.wasmtime.ImportType;
import io.github.kawamuray.wasmtime.Module;
import io.github.kawamuray.wasmtime.Store;
import io.github.kawamuray.wasmtime.Func;
import io.github.kawamuray.wasmtime.Val;
import io.github.kawamuray.wasmtime.Caller;
import io.github.kawamuray.wasmtime.Memory;

import java.util.ArrayList;
import java.util.List;
import jakarta.annotation.PreDestroy;

// @Service
public class WasmRuntimeService {
    private static final Logger log = LoggerFactory.getLogger(WasmRuntimeService.class);

    @FunctionalInterface
    private interface HostProvider {
        WasmLwm2mHost get();
    }

    private enum InstanceScope {
        GROUP,
        DEVICE
    }

    private static final class InstanceKey {
        private final InstanceScope scope;
        private final String identifier;

        private InstanceKey(InstanceScope scope, String identifier) {
            this.scope = scope;
            this.identifier = identifier;
        }

        static InstanceKey group(String groupId) {
            return new InstanceKey(InstanceScope.GROUP, groupId);
        }

        static InstanceKey device(String deviceKey) {
            return new InstanceKey(InstanceScope.DEVICE, deviceKey);
        }

        InstanceScope scope() {
            return scope;
        }

        String identifier() {
            return identifier;
        }

        @Override
        public boolean equals(Object o) {
            if (this == o) return true;
            if (o == null || getClass() != o.getClass()) return false;
            InstanceKey that = (InstanceKey) o;
            return scope == that.scope && java.util.Objects.equals(identifier, that.identifier);
        }

        @Override
        public int hashCode() {
            return java.util.Objects.hash(scope, identifier);
        }

        @Override
        public String toString() {
            return scope + "::" + identifier;
        }
    }

    private static final class DeviceContext {
        private final InstanceKey instanceKey;
        private final HostProvider hostProvider;
        private final Map<InstanceKey, String> registrationObjects;

        private DeviceContext(InstanceKey instanceKey, HostProvider hostProvider, Map<InstanceKey, String> registrationObjects) {
            this.instanceKey = instanceKey;
            this.hostProvider = hostProvider;
            this.registrationObjects = registrationObjects;
        }
    }

    private record MavenCoordinates(String groupId, String artifactId, String version, String classifier, String extension,
                                    String repoUrl, String localRepo, String sha256) {
    }

    private static final class DeviceInstance {
        private final Store<DeviceContext> store;
        private final Instance instance;

        private DeviceInstance(Store<DeviceContext> store, Instance instance) {
            this.store = store;
            this.instance = instance;
        }

        private void close() {
            instance.close();
            store.close();
        }
    }

    private final Map<InstanceKey, DeviceInstance> instancesByKey = new ConcurrentHashMap<>();
    private final Map<InstanceKey, String> registrationObjectsByKey = new ConcurrentHashMap<>();
    private final Map<MavenCoordinates, Module> moduleCache = new ConcurrentHashMap<>();
    private final Set<MavenCoordinates> loggedImportModules = ConcurrentHashMap.newKeySet();

    private static final Map<String, MavenCoordinates> GROUP_MODULES = new ConcurrentHashMap<>();
    private static final Map<String, MavenCoordinates> DEVICE_MODULES = new ConcurrentHashMap<>();

    public static void registerGroupModule(String groupId, String artifactGroupId, String artifactId, String version,
                                           String classifier, String extension, String repoUrl, String sha256) {
        GROUP_MODULES.put(groupId, new MavenCoordinates(artifactGroupId, artifactId, version, classifier, extension, repoUrl, null, sha256));
    }

    public static void registerDeviceModule(String deviceId, String artifactGroupId, String artifactId, String version,
                                            String classifier, String extension, String repoUrl, String sha256) {
        DEVICE_MODULES.put(deviceId, new MavenCoordinates(artifactGroupId, artifactId, version, classifier, extension, repoUrl, null, sha256));
    }

    private volatile WasmLwm2mHost lwm2mHost;

    /** Inject host LwM2M operations used by wasm imports. */
    public void setLwm2mHost(WasmLwm2mHost lwm2mHost) {
        this.lwm2mHost = lwm2mHost;
    }

    @Value("${wasmer.module-sha256:}")
    private String moduleSha256;

    @Value("${wasmer.module-maven.groupId:ai.edgez}")
    private String mavenGroupId;

    @Value("${wasmer.module-maven.artifactId:edgeOS}")
    private String mavenArtifactId;

    @Value("${wasmer.module-maven.version:0.0.1-SNAPSHOT}")
    private String mavenVersion;

    @Value("${wasmer.module-maven.classifier:wasm}")
    private String mavenClassifier;

    @Value("${wasmer.module-maven.extension:wasm}")
    private String mavenExtension;

    @Value("${wasmer.module-maven.repo-url:}")
    private String mavenRepoUrl;

    @Value("${wasmer.module-maven.local-repo:}")
    private String mavenLocalRepo;

    @Value("${wasmer.cache-dir:}")
    private String cacheDir;

    @Value("${wasmer.entry-function:}")
    private String entryFunction;

    @Value("${wasmer.auto-start.enabled:true}")
    private boolean autoStartEnabled;

    private Engine engine;

    /**
     * Ensure the configured Wasm module is instantiated for a device. If already instantiated, do nothing.
     */
    public boolean ensureModuleRunning(String deviceKey) {
        return ensureModuleRunning(InstanceKey.device(deviceKey));
    }

    private boolean ensureModuleRunning(InstanceKey instanceKey) {
        if (!autoStartEnabled) {
            log.debug("Wasm auto-start disabled; skipping for {}", instanceKey);
            return false;
        }
        if (instanceKey == null || instanceKey.identifier() == null || instanceKey.identifier().isBlank()) {
            log.warn("Cannot start Wasm module: instance key missing");
            return false;
        }
        ModuleLocation location = resolveModule(instanceKey);
        if (location == null) {
            return false;
        }

        java.util.concurrent.atomic.AtomicBoolean created = new java.util.concurrent.atomic.AtomicBoolean(false);
        instancesByKey.compute(instanceKey, (key, existing) -> {
            if (existing != null) {
                return existing;
            }
            try {
                Module module = loadModule(location);
                DeviceInstance deviceInstance = createInstance(location.path(), location.coordinates(), instanceKey, module);
                invokeEntryIfPresent(deviceInstance);
                log.info("Started Wasm instance for {} using {}", instanceKey, location.path());
                created.set(true);
                return deviceInstance;
            } catch (Exception e) {
                log.error("Failed to start Wasm instance for {}: {}", instanceKey, e.getMessage(), e);
                return existing;
            }
        });

        return created.get();
    }

    private synchronized DeviceInstance createInstance(Path wasmPath, MavenCoordinates coordinates, InstanceKey instanceKey, Module module) throws IOException {
        if (engine == null) {
            engine = new Engine();
        }
        
        Store<DeviceContext> store = new Store<>(new DeviceContext(instanceKey, () -> lwm2mHost, registrationObjectsByKey), engine);

        ImportType[] expectedImports = module.imports();
        if (loggedImportModules.add(coordinates)) {
            log.info("Wasm module expects {} imports:", expectedImports.length);
            for (ImportType imp : expectedImports) {
                if (imp.type() == ImportType.Type.FUNC) {
                    log.info("  - {}::{} func params={} results={}",
                            imp.module(),
                            imp.name(),
                            java.util.Arrays.toString(imp.func().getParams()),
                            java.util.Arrays.toString(imp.func().getResults()));
                } else {
                    log.info("  - {}::{} {}", imp.module(), imp.name(), imp.type());
                }
            }
        }

        List<Extern> imports = new ArrayList<>(expectedImports.length);
        for (ImportType imp : expectedImports) {
            imports.add(buildImport(store, imp));
        }
        
        Instance instance = new Instance(store, module, imports);
        return new DeviceInstance(store, instance);
    }

    private Extern buildImport(Store<DeviceContext> store, ImportType imp) {
        if (imp.type() != ImportType.Type.FUNC) {
            throw new IllegalStateException(
                    "Unsupported Wasm import type " + imp.type() + " for " + imp.module() + "::" + imp.name());
        }

        String key = imp.module() + "::" + imp.name();

        Func func;
        switch (key) {
            case "teavm::currentTimeMillis" -> func = new Func(store, imp.func(), (caller, args, results) -> {
                long now = System.currentTimeMillis();
                if (results.length == 0) {
                    return;
                }
                if (results.length != 1) {
                    throw new IllegalStateException("Unexpected results arity for " + key + ": " + results.length);
                }
                results[0] = switch (results[0].getType()) {
                    case I32 -> Val.fromI32((int) now);
                    case I64 -> Val.fromI64(now);
                    case F32 -> Val.fromF32((float) now);
                    case F64 -> Val.fromF64((double) now);
                    default -> throw new IllegalStateException("Unsupported result type for " + key + ": " + results[0].getType());
                };
            });
            case "teavm::logString" -> func = new Func(store, imp.func(), (caller, args, results) -> {
                log.debug("[Wasm/teavm.logString] args={}", formatArgs(args));
            });
            case "teavm::logInt" -> func = new Func(store, imp.func(), (caller, args, results) -> {
                log.info("[Wasm/teavm.logInt] args={}", formatArgs(args));
            });
            case "teavm::logOutOfMemory" -> func = new Func(store, imp.func(), (caller, args, results) -> {
                log.error("[Wasm/teavm.logOutOfMemory] OOM args={}", formatArgs(args));
            });
            case "env::log" -> func = new Func(store, imp.func(), (caller, args, results) -> {
                log.info("[Wasm/env.log] args={}", formatArgs(args));
            });
            case "env::getRegisteredObjects", "lwm2m::getRegisteredObjects" -> func = new Func(store, imp.func(), (caller, args, results) -> {
                if (args.length != 2) {
                    throw new IllegalStateException("Unsupported signature for " + key + ": args=" + args.length);
                }
                int outPtr = requireI32(args[0], key, 0);
                int outCap = requireI32(args[1], key, 1);
                String json = store.data().registrationObjects.getOrDefault(store.data().instanceKey, "");
                int written = writeBytes(store, (Caller<DeviceContext>) caller, outPtr, outCap, json.getBytes(StandardCharsets.UTF_8));
                writeI32Result(results, written, key);
            });
            case "env::read", "lwm2m::read" -> func = new Func(store, imp.func(), (caller, args, results) -> {
                handleRead(store, (Caller<DeviceContext>) caller, args, results, key);
            });
            case "env::write", "lwm2m::write" -> func = new Func(store, imp.func(), (caller, args, results) -> {
                handleWrite(store, (Caller<DeviceContext>) caller, args, results, key);
            });
            case "env::observe", "lwm2m::observe" -> func = new Func(store, imp.func(), (caller, args, results) -> {
                handleObserve(store, (Caller<DeviceContext>) caller, args, results, key);
            });
            case "env::cancelObserve", "env::cancel_observe", "lwm2m::cancelObserve", "lwm2m::cancel_observe" -> func = new Func(store, imp.func(), (caller, args, results) -> {
                handleCancelObserve(store, (Caller<DeviceContext>) caller, args, results, key);
            });
            default -> throw new IllegalStateException("No host import provided for " + key);
        }

        return Extern.fromFunc(func);
    }

    private static void handleRead(Store<DeviceContext> store, Caller<DeviceContext> caller, Val[] args, Val[] results, String key) {
        int rc = -1;
        try {
            WasmLwm2mHost host = store.data().hostProvider.get();
            if (host == null) {
                throw new IllegalStateException("No WasmLwm2mHost configured");
            }
            String deviceKey = store.data().instanceKey.identifier();

            String path;
            int outPtr;
            int outCap;

            if (args.length == 5) {
                // (objId, instId, resId, outPtr, outCap)
                int objId = requireI32(args[0], key, 0);
                int instId = requireI32(args[1], key, 1);
                int resId = requireI32(args[2], key, 2);
                path = objId + "/" + instId + "/" + resId;
                outPtr = requireI32(args[3], key, 3);
                outCap = requireI32(args[4], key, 4);
            } else if (args.length == 4) {
                // (pathPtr, pathLen, outPtr, outCap)
                int pathPtr = requireI32(args[0], key, 0);
                int pathLen = requireI32(args[1], key, 1);
                outPtr = requireI32(args[2], key, 2);
                outCap = requireI32(args[3], key, 3);
                path = readUtf8(store, caller, pathPtr, pathLen);
            } else {
                throw new IllegalStateException("Unsupported signature for " + key + ": args=" + args.length);
            }

            String payload = host.read(deviceKey, path);
            byte[] bytes = payload != null ? payload.getBytes(StandardCharsets.UTF_8) : new byte[0];
            int written = writeBytes(store, caller, outPtr, outCap, bytes);
            rc = written;
        } catch (Exception e) {
            log.warn("[Wasm/{}] read failed: {} args={}", key, e.getMessage(), formatArgs(args));
            rc = -1;
        }

        writeI32Result(results, rc, key);
    }

    private static void handleWrite(Store<DeviceContext> store, Caller<DeviceContext> caller, Val[] args, Val[] results, String key) {
        int rc = -1;
        try {
            WasmLwm2mHost host = store.data().hostProvider.get();
            if (host == null) {
                throw new IllegalStateException("No WasmLwm2mHost configured");
            }
            String deviceKey = store.data().instanceKey.identifier();

            String path;
            int payloadPtr;
            int payloadLen;

            if (args.length == 5) {
                // (objId, instId, resId, payloadPtr, payloadLen)
                int objId = requireI32(args[0], key, 0);
                int instId = requireI32(args[1], key, 1);
                int resId = requireI32(args[2], key, 2);
                path = objId + "/" + instId + "/" + resId;
                payloadPtr = requireI32(args[3], key, 3);
                payloadLen = requireI32(args[4], key, 4);
            } else if (args.length == 4) {
                // (pathPtr, pathLen, payloadPtr, payloadLen)
                int pathPtr = requireI32(args[0], key, 0);
                int pathLen = requireI32(args[1], key, 1);
                payloadPtr = requireI32(args[2], key, 2);
                payloadLen = requireI32(args[3], key, 3);
                path = readUtf8(store, caller, pathPtr, pathLen);
            } else {
                throw new IllegalStateException("Unsupported signature for " + key + ": args=" + args.length);
            }

            byte[] payload = readBytes(store, caller, payloadPtr, payloadLen);
            host.write(deviceKey, path, payload);
            rc = 0;
        } catch (Exception e) {
            log.warn("[Wasm/{}] write failed: {} args={}", key, e.getMessage(), formatArgs(args));
            rc = -1;
        }

        writeI32Result(results, rc, key);
    }

    private static void handleObserve(Store<DeviceContext> store, Caller<DeviceContext> caller, Val[] args, Val[] results, String key) {
        int rc = -1;
        try {
            WasmLwm2mHost host = store.data().hostProvider.get();
            if (host == null) {
                throw new IllegalStateException("No WasmLwm2mHost configured");
            }
            String deviceKey = store.data().instanceKey.identifier();

            String path;
            if (args.length == 3) {
                int objId = requireI32(args[0], key, 0);
                int instId = requireI32(args[1], key, 1);
                int resId = requireI32(args[2], key, 2);
                path = objId + "/" + instId + "/" + resId;
            } else if (args.length == 2) {
                int pathPtr = requireI32(args[0], key, 0);
                int pathLen = requireI32(args[1], key, 1);
                path = readUtf8(store, caller, pathPtr, pathLen);
            } else {
                throw new IllegalStateException("Unsupported signature for " + key + ": args=" + args.length);
            }

            host.observe(deviceKey, path);
            rc = 0;
        } catch (Exception e) {
            log.warn("[Wasm/{}] observe failed: {} args={}", key, e.getMessage(), formatArgs(args));
            rc = -1;
        }

        writeI32Result(results, rc, key);
    }

    private static void handleCancelObserve(Store<DeviceContext> store, Caller<DeviceContext> caller, Val[] args, Val[] results, String key) {
        int rc = -1;
        try {
            WasmLwm2mHost host = store.data().hostProvider.get();
            if (host == null) {
                throw new IllegalStateException("No WasmLwm2mHost configured");
            }
            String deviceKey = store.data().instanceKey.identifier();

            String path;
            if (args.length == 3) {
                int objId = requireI32(args[0], key, 0);
                int instId = requireI32(args[1], key, 1);
                int resId = requireI32(args[2], key, 2);
                path = objId + "/" + instId + "/" + resId;
            } else if (args.length == 2) {
                int pathPtr = requireI32(args[0], key, 0);
                int pathLen = requireI32(args[1], key, 1);
                path = readUtf8(store, caller, pathPtr, pathLen);
            } else {
                throw new IllegalStateException("Unsupported signature for " + key + ": args=" + args.length);
            }

            host.cancelObserve(deviceKey, path);
            rc = 0;
        } catch (Exception e) {
            log.warn("[Wasm/{}] cancelObserve failed: {} args={}", key, e.getMessage(), formatArgs(args));
            rc = -1;
        }

        writeI32Result(results, rc, key);
    }

    private static int requireI32(Val v, String key, int index) {
        if (v.getType() != Val.Type.I32) {
            throw new IllegalStateException("Expected I32 arg at index " + index + " for " + key + ", got " + v.getType());
        }
        return v.i32();
    }

    private static Memory requireMemory(Store<DeviceContext> store, Caller<DeviceContext> caller) {
        Optional<Extern> extern = caller.getExport("memory");
        if (extern.isEmpty()) {
            throw new IllegalStateException("Wasm module does not export memory as 'memory'");
        }
        return extern.get().memory();
    }

    private static String readUtf8(Store<DeviceContext> store, Caller<DeviceContext> caller, int ptr, int len) {
        if (len == 0) {
            return "";
        }
        byte[] bytes = readBytes(store, caller, ptr, len);
        return new String(bytes, StandardCharsets.UTF_8);
    }

    private static byte[] readBytes(Store<DeviceContext> store, Caller<DeviceContext> caller, int ptr, int len) {
        if (ptr < 0 || len < 0) {
            throw new IllegalArgumentException("Negative ptr/len");
        }
        Memory mem = requireMemory(store, caller);
        ByteBuffer buf = mem.buffer(store);
        if ((long) ptr + (long) len > buf.capacity()) {
            throw new IndexOutOfBoundsException("Out of bounds memory read: ptr=" + ptr + " len=" + len + " cap=" + buf.capacity());
        }
        byte[] out = new byte[len];
        ByteBuffer dup = buf.duplicate();
        dup.position(ptr);
        dup.get(out);
        return out;
    }

    private static int writeBytes(Store<DeviceContext> store, Caller<DeviceContext> caller, int ptr, int cap, byte[] data) {
        if (ptr < 0 || cap < 0) {
            throw new IllegalArgumentException("Negative ptr/cap");
        }
        Memory mem = requireMemory(store, caller);
        ByteBuffer buf = mem.buffer(store);
        if ((long) ptr + (long) cap > buf.capacity()) {
            throw new IndexOutOfBoundsException("Out of bounds memory write: ptr=" + ptr + " cap=" + cap + " memCap=" + buf.capacity());
        }
        int n = Math.min(cap, data != null ? data.length : 0);
        if (n == 0) {
            return 0;
        }
        ByteBuffer dup = buf.duplicate();
        dup.position(ptr);
        dup.put(data, 0, n);
        return n;
    }

    private static void writeI32Result(Val[] results, int value, String key) {
        if (results == null || results.length == 0) {
            return;
        }
        if (results.length != 1) {
            throw new IllegalStateException("Unexpected results arity for " + key + ": " + results.length);
        }
        if (results[0].getType() != Val.Type.I32) {
            throw new IllegalStateException("Unsupported result type for " + key + ": " + results[0].getType());
        }
        results[0] = Val.fromI32(value);
    }

    private static String formatArgs(Val[] args) {
        if (args == null || args.length == 0) {
            return "[]";
        }
        StringBuilder sb = new StringBuilder("[");
        for (int i = 0; i < args.length; i++) {
            Val v = args[i];
            if (i > 0) sb.append(", ");
            sb.append(v.getType()).append('=');
            switch (v.getType()) {
                case I32 -> sb.append(v.i32());
                case I64 -> sb.append(v.i64());
                case F32 -> sb.append(v.f32());
                case F64 -> sb.append(v.f64());
                default -> sb.append(String.valueOf(v.getValue()));
            }
        }
        sb.append(']');
        return sb.toString();
    }

    private ModuleLocation resolveModule(InstanceKey instanceKey) {
        MavenCoordinates coords = resolveCoordinatesFor(instanceKey);
        if (coords == null) {
            log.warn("No Maven coordinates configured for {}", instanceKey);
            return null;
        }
        Path path = resolveFromMaven(coords);
        if (path == null) {
            return null;
        }
        return new ModuleLocation(coords, path);
    }

    private MavenCoordinates resolveCoordinatesFor(InstanceKey instanceKey) {
        MavenCoordinates coords;
        if (instanceKey.scope() == InstanceScope.GROUP) {
            coords = GROUP_MODULES.get(instanceKey.identifier());
        } else {
            coords = DEVICE_MODULES.get(instanceKey.identifier());
        }

        if (coords != null) {
            return coords;
        }

        // Fallback to default Maven coordinates from properties
        return new MavenCoordinates(
                mavenGroupId,
                mavenArtifactId,
                mavenVersion,
                mavenClassifier,
                mavenExtension,
                mavenRepoUrl,
                mavenLocalRepo,
                moduleSha256);
    }

    private Path resolveFromMaven(MavenCoordinates coords) {
        try {
            String repoUrl = (coords.repoUrl() != null && !coords.repoUrl().isBlank()) ? coords.repoUrl() 
                : (mavenRepoUrl != null && !mavenRepoUrl.isBlank()) ? mavenRepoUrl 
                : null;
            
            String localRepo = (coords.localRepo() != null && !coords.localRepo().isBlank()) ? coords.localRepo() : mavenLocalRepo;
            Path localRepoPath = (localRepo == null || localRepo.isBlank())
                    ? Path.of(System.getProperty("user.home"), ".m2", "repository")
                    : Path.of(localRepo);

            MavenWasmResolver resolver = new MavenWasmResolver();
            Path artifactPath = resolver.resolve(
                    coords.groupId(),
                    coords.artifactId(),
                    coords.version(),
                    coords.classifier(),
                    coords.extension(),
                    repoUrl,
                    localRepoPath);

            String sha = coords.sha256() != null && !coords.sha256().isBlank() ? coords.sha256() : moduleSha256;
            if (sha != null && !sha.isBlank()) {
                verifySha256(artifactPath, sha);
            }

            Path targetDir = (cacheDir == null || cacheDir.isBlank())
                    ? Path.of(System.getProperty("java.io.tmpdir"), "wasm-cache")
                    : Path.of(cacheDir);
            Files.createDirectories(targetDir);
            Path cached = targetDir.resolve(artifactPath.getFileName());
            Files.copy(artifactPath, cached, StandardCopyOption.REPLACE_EXISTING);
            log.info("Resolved Wasm from {} into {}", repoUrl != null ? "Maven repo " + repoUrl : "local repository", cached);
            return cached;
        } catch (Exception e) {
            log.error("Failed to resolve Wasm via Maven coordinates {}:{}:{}:{}: {}",
                    coords.groupId(), coords.artifactId(), coords.version(), coords.classifier(), e.getMessage(), e);
            return null;
        }
    }

    private void verifySha256(Path target, String expectedHex) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        byte[] bytes = Files.readAllBytes(target);
        String actual = toHex(digest.digest(bytes));
        if (!actual.equalsIgnoreCase(expectedHex.trim())) {
            throw new IOException("SHA-256 mismatch: expected " + expectedHex + " got " + actual);
        }
    }

    private String toHex(byte[] bytes) {
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            sb.append(Character.forDigit((b >> 4) & 0xF, 16));
            sb.append(Character.forDigit((b) & 0xF, 16));
        }
        return sb.toString();
    }

    private Module loadModule(ModuleLocation location) throws IOException {
        if (engine == null) {
            engine = new Engine();
        }
        return moduleCache.computeIfAbsent(location.coordinates(), coords -> {
            try {
                byte[] wasmBytes = Files.readAllBytes(location.path());
                Module m = new Module(engine, wasmBytes);
                log.info("Loaded Wasm module {}:{}:{} from {}", coords.groupId(), coords.artifactId(), coords.version(), location.path());
                return m;
            } catch (IOException e) {
                throw new RuntimeException(e);
            }
        });
    }

    private record ModuleLocation(MavenCoordinates coordinates, Path path) {}

    private void invokeEntryIfPresent(DeviceInstance deviceInstance) {
        if (entryFunction == null || entryFunction.isBlank()) {
            return;
        }
        deviceInstance.instance.getFunc(deviceInstance.store, entryFunction)
                .ifPresentOrElse(func -> func.call(deviceInstance.store),
                        () -> log.warn("Entry function '{}' not found in module exports", entryFunction));
    }

    public boolean callMain(String deviceKey) {
        return callMain(InstanceKey.device(deviceKey));
    }

    public boolean callMain(InstanceKey key) {
        return callExport0(key, "main");
    }

    public boolean callRegister(String deviceKey) {
        return callRegister(InstanceKey.device(deviceKey));
    }

    public boolean callRegister(InstanceKey key) {
        return callExport0(key, "register");
    }

    public boolean callUnregister(String deviceKey) {
        return callUnregister(InstanceKey.device(deviceKey));
    }

    public boolean callUnregister(InstanceKey key) {
        return callExport0(key, "unregister");
    }

    public boolean callEvent(String deviceKey) {
        return callEvent(InstanceKey.device(deviceKey));
    }

    public boolean callEvent(InstanceKey key) {
        return callExport0(key, "event");
    }

    private boolean callExport0(InstanceKey key, String exportName) {
        DeviceInstance deviceInstance = instancesByKey.get(key);
        if (deviceInstance == null) {
            return false;
        }
        try {
            return deviceInstance.instance.getFunc(deviceInstance.store, exportName)
                    .map(func -> {
                        try {
                            func.call(deviceInstance.store);
                            return true;
                        } catch (Exception e) {
                            log.error("Failed calling wasm export '{}' for {}: {}", exportName, key, e.getMessage(), e);
                            return false;
                        }
                    })
                    .orElseGet(() -> {
                        log.debug("Wasm export '{}' not found for {}", exportName, key);
                        return false;
                    });
        } catch (Exception e) {
            log.error("Failed resolving wasm export '{}' for {}: {}", exportName, key, e.getMessage(), e);
            return false;
        }
    }

    public void stopIfRunning(String deviceKey) {
        stopIfRunning(InstanceKey.device(deviceKey));
    }

    public void stopIfRunning(InstanceKey key) {
        DeviceInstance deviceInstance = instancesByKey.remove(key);
        registrationObjectsByKey.remove(key);
        if (deviceInstance != null) {
            deviceInstance.close();
            log.info("Closed Wasm instance for {}", key);
        }
    }

    public void handleEndpointRegistered(String endpoint, String registeredObjectsJson) {
        if (endpoint == null || endpoint.isBlank()) {
            log.warn("Cannot start Wasm for empty endpoint");
            return;
        }

        String objectsValue = registeredObjectsJson == null ? "" : registeredObjectsJson;
        extractGroupId(endpoint).ifPresent(groupId -> ensureAndRegisterLifecycle(InstanceKey.group(groupId), objectsValue));
        ensureAndRegisterLifecycle(InstanceKey.device(endpoint), objectsValue);
    }

    public void handleEndpointDeregistered(String endpoint) {
        if (endpoint == null || endpoint.isBlank()) {
            return;
        }
        InstanceKey deviceKey = InstanceKey.device(endpoint);
        if (instancesByKey.containsKey(deviceKey)) {
            callUnregister(deviceKey);
            stopIfRunning(deviceKey);
        }
    }

    private void ensureAndRegisterLifecycle(InstanceKey key, String registeredObjectsJson) {
        registrationObjectsByKey.put(key, registeredObjectsJson == null ? "" : registeredObjectsJson);
        boolean created = ensureModuleRunning(key);
        if (created) {
            callMain(key);
            callRegister(key);
        }
    }

    private java.util.Optional<String> extractGroupId(String endpoint) {
        int dash = endpoint.indexOf('-');
        if (dash <= 0) {
            log.warn("Endpoint '{}' does not include group prefix; skipping group wasm", endpoint);
            return java.util.Optional.empty();
        }
        return java.util.Optional.of(endpoint.substring(0, dash));
    }

    @PreDestroy
    public void shutdown() {
        instancesByKey.keySet().forEach(this::stopIfRunning);
        moduleCache.values().forEach(Module::close);
        if (engine != null) {
            engine.close();
        }
    }
}
