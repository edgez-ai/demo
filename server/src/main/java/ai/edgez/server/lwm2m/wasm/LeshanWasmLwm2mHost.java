package ai.edgez.server.lwm2m.wasm;

import java.time.Duration;

import org.eclipse.leshan.core.observation.Observation;
import org.eclipse.leshan.core.observation.SingleObservation;
import org.eclipse.leshan.core.request.ObserveRequest;
import org.eclipse.leshan.core.request.ReadRequest;
import org.eclipse.leshan.core.request.WriteRequest;
import org.eclipse.leshan.core.response.LwM2mResponse;
import org.eclipse.leshan.server.LeshanServer;
import org.eclipse.leshan.server.registration.Registration;

/**
 * Default {@link WasmLwm2mHost} implementation backed by a Leshan server.
 */
public final class LeshanWasmLwm2mHost implements WasmLwm2mHost {
    private static final long DEFAULT_TIMEOUT_MS = Duration.ofSeconds(5).toMillis();

    private final LeshanServer server;

    public LeshanWasmLwm2mHost(LeshanServer server) {
        this.server = server;
    }

    @Override
    public String read(String deviceKey, String path) throws Exception {
        Registration registration = server.getRegistrationService().getByEndpoint(deviceKey);
        if (registration == null) {
            throw new IllegalStateException("Unknown device endpoint: " + deviceKey);
        }
        ReadRequest request = new ReadRequest(path);
        LwM2mResponse response = server.send(registration, request, DEFAULT_TIMEOUT_MS);
        return response != null ? response.toString() : "";
    }

    @Override
    public void write(String deviceKey, String path, byte[] payload) throws Exception {
        Registration registration = server.getRegistrationService().getByEndpoint(deviceKey);
        if (registration == null) {
            throw new IllegalStateException("Unknown device endpoint: " + deviceKey);
        }
        // Minimal default: write as UTF-8 text (common for demos).
        String value = payload != null ? new String(payload, java.nio.charset.StandardCharsets.UTF_8) : "";

        int[] ids = parseResourcePath(path);
        // (objectId, instanceId, resourceId, value)
        WriteRequest request = new WriteRequest(ids[0], ids[1], ids[2], value);
        server.send(registration, request, DEFAULT_TIMEOUT_MS);
    }

    private static int[] parseResourcePath(String path) {
        if (path == null || path.isBlank()) {
            throw new IllegalArgumentException("Path is empty");
        }
        String[] parts = path.split("/");
        if (parts.length != 3) {
            throw new IllegalArgumentException("Expected resource path like '3/0/0' but got: " + path);
        }
        try {
            return new int[] { Integer.parseInt(parts[0]), Integer.parseInt(parts[1]), Integer.parseInt(parts[2]) };
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("Non-numeric resource path: " + path, e);
        }
    }

    @Override
    public void observe(String deviceKey, String path) throws Exception {
        Registration registration = server.getRegistrationService().getByEndpoint(deviceKey);
        if (registration == null) {
            throw new IllegalStateException("Unknown device endpoint: " + deviceKey);
        }
        ObserveRequest request = new ObserveRequest(path);
        server.send(registration, request, DEFAULT_TIMEOUT_MS);
    }

    @Override
    public void cancelObserve(String deviceKey, String path) throws Exception {
        Registration registration = server.getRegistrationService().getByEndpoint(deviceKey);
        if (registration == null) {
            throw new IllegalStateException("Unknown device endpoint: " + deviceKey);
        }

        for (Observation obs : server.getObservationService().getObservations(registration)) {
            if (obs instanceof SingleObservation single && single.getPath().toString().equals(path)) {
                server.getObservationService().cancelObservation(obs);
                return;
            }
        }

        // No-op if there is no matching observation.
    }
}
