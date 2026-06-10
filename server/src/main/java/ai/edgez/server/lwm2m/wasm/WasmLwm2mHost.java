package ai.edgez.server.lwm2m.wasm;

/**
 * Host-side LwM2M operations exposed to WebAssembly as imports.
 *
 * Conventions used by {@link WasmRuntimeService}:
 * - return 0 on success
 * - return negative values on error
 */
public interface WasmLwm2mHost {
    /**
     * Reads a resource identified by LwM2M path (e.g. "3/0/0").
     *
     * @return UTF-8 string payload for wasm to consume
     */
    String read(String deviceKey, String path) throws Exception;

    /** Writes a resource identified by LwM2M path (e.g. "3/0/0"). */
    void write(String deviceKey, String path, byte[] payload) throws Exception;

    /** Starts observing a resource identified by LwM2M path (e.g. "3/0/0"). */
    void observe(String deviceKey, String path) throws Exception;

    /** Cancels an observation for a resource identified by LwM2M path (e.g. "3/0/0"). */
    void cancelObserve(String deviceKey, String path) throws Exception;
}
