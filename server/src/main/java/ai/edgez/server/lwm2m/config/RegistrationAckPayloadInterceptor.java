package ai.edgez.server.lwm2m.config;

import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

import org.eclipse.californium.core.coap.CoAP;
import org.eclipse.californium.core.coap.EmptyMessage;
import org.eclipse.californium.core.coap.MediaTypeRegistry;
import org.eclipse.californium.core.coap.Request;
import org.eclipse.californium.core.coap.Response;
import org.eclipse.californium.core.network.interceptors.MessageInterceptor;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;

import ai.edgez.server.lwm2m.util.LuciConfigService;
import ai.edgez.server.lwm2m.util.SecretKeyService;

/**
 * Adds a custom JSON payload to registration ACKs (2.01 Created on /rd) and
 * carries parsed URI query parameters from the registration request.
 */
public class RegistrationAckPayloadInterceptor implements MessageInterceptor {

    private static final Logger log = LoggerFactory.getLogger(RegistrationAckPayloadInterceptor.class);

    private static final int DEFAULT_SAMPLE_VERSION = 1;
    private static final int DEFAULT_SAMPLING_RATE = 60;
    private static final int DEFAULT_REPORT_RATE = 300;
    private static final int DEFAULT_SEND_ACK_TIMEOUT_MS = 5000;
    private static final int DEFAULT_SEND_RETRY_DELAY_MS = 1000;
    private static final int DEFAULT_SEND_RETRY_COUNT = 3;
    private static final long DEFAULT_MISSING_SYNC_DELAY_MS = 200L;
    private static final String DEFAULT_SLEEP_MODE = "no";
    private static final String DEFAULT_RESOLUTION = "320x240";

    private final ObjectMapper mapper = new ObjectMapper();
    private final ConcurrentMap<String, Map<String, String>> requestQueryByToken = new ConcurrentHashMap<>();

    private final boolean enabled;
    private final boolean includeQueryParams;
    private final int timeSyncIntervalSec;
    private final SecretKeyService secretKeyService;
    private final LuciConfigService luciConfigService;
    private final MissingScriptSyncTrigger missingScriptSyncTrigger;
    private final long serverStartEpochMs;

    public RegistrationAckPayloadInterceptor(boolean enabled,
                                             boolean includeQueryParams,
                                             int timeSyncIntervalSec,
                                             SecretKeyService secretKeyService,
                                             LuciConfigService luciConfigService,
                                             MissingScriptSyncTrigger missingScriptSyncTrigger) {
        this.enabled = enabled;
        this.includeQueryParams = includeQueryParams;
        this.timeSyncIntervalSec = timeSyncIntervalSec;
        this.secretKeyService = secretKeyService;
        this.luciConfigService = luciConfigService;
        this.missingScriptSyncTrigger = missingScriptSyncTrigger;
        this.serverStartEpochMs = Instant.now().toEpochMilli();
    }

    @Override
    public void receiveRequest(Request request) {
        if (!enabled || request == null || !isRegistrationCreate(request)) {
            return;
        }

        Map<String, String> registrationQuery = parseUriQuery(request);
        requestQueryByToken.put(request.getTokenString(), registrationQuery);
        log.info("registration-ack request captured: token={} path={} clientArgs={}",
                request.getTokenString(),
                request.getOptions() != null ? request.getOptions().getUriPathString() : "",
                registrationQuery);
    }

    @Override
    public void sendResponse(Response response) {
        if (!enabled || response == null || !isRegistrationCreated(response)) {
            return;
        }

        Map<String, String> registrationQuery = requestQueryByToken.remove(response.getTokenString());
        if (registrationQuery == null) {
            registrationQuery = Map.of();
        }

        Map<String, Object> payload = new LinkedHashMap<>();
        Instant now = Instant.now();
        long nowMs = now.toEpochMilli();
        long uptimeMs = Math.max(0L, nowMs - serverStartEpochMs);

        String endpoint = trimToEmpty(registrationQuery.get("ep"));
        SampleConfigResolution configResolution = loadSampleConfig(endpoint);
        SampleConfig sampleConfig = configResolution.sampleConfig();

        log.info("registration-ack triggered: token={} endpoint={} clientArgs={} configSource={} zone={} sampleVersion={} samplingEnabled={} samplingRate={} reportRate={} i2cList={} rs485List={}",
            response.getTokenString(),
            endpoint,
            registrationQuery,
            configResolution.source(),
            configResolution.zone(),
            sampleConfig.version(),
            sampleConfig.samplingEnabled(),
            sampleConfig.samplingRate(),
            sampleConfig.reportRate(),
            sampleConfig.sensorUartI2c(),
            sampleConfig.sensorRs485());

        scheduleMissingScriptSyncFromQuery(endpoint, registrationQuery);

        payload.put("server_uptime_ms", uptimeMs);
        payload.put("server_sec_of_year", computeServerSecOfYear(now));
        payload.put("server_version", resolveServerVersion(registrationQuery, sampleConfig.serverVersion()));

        payload.put("version", sampleConfig.version());
        payload.put("sampling_enabled", sampleConfig.samplingEnabled() ? 1 : 0);
        payload.put("sampling_rate", sampleConfig.samplingRate());
        payload.put("report_rate", sampleConfig.reportRate());
        payload.put("sleep_mode", sampleConfig.sleepMode());
        payload.put("sensor_uart_i2c", sampleConfig.sensorUartI2c());
        payload.put("sensor_rs485", sampleConfig.sensorRs485());
        payload.put("resolution", sampleConfig.resolution());

        try {
            String ackJson = mapper.writeValueAsString(payload);
            int ackBytes = ackJson.getBytes(StandardCharsets.UTF_8).length;
            log.info("registration-ack payload size={} bytes", ackBytes);

            response.setPayload(ackJson);
            response.getOptions().setContentFormat(MediaTypeRegistry.APPLICATION_JSON);
        } catch (JsonProcessingException e) {
            // Keep protocol-level registration success response even if payload serialization fails.
        }
    }

    @Override
    public void sendRequest(Request request) {
        // no-op
    }

    @Override
    public void sendEmptyMessage(EmptyMessage message) {
        // no-op
    }

    @Override
    public void receiveResponse(Response response) {
        // no-op
    }

    @Override
    public void receiveEmptyMessage(EmptyMessage message) {
        // no-op
    }

    private static boolean isRegistrationCreate(Request request) {
        if (request.getCode() != CoAP.Code.POST) {
            return false;
        }
        String path = request.getOptions().getUriPathString();
        if (path == null) {
            return false;
        }
        String normalized = path.startsWith("/") ? path.substring(1) : path;
        return "rd".equals(normalized);
    }

    private static boolean isRegistrationCreated(Response response) {
        if (response.getCode() != CoAP.ResponseCode.CREATED) {
            return false;
        }
        return response.getOptions() != null && response.getOptions().getLocationPathCount() > 0;
    }

    private static Map<String, String> parseUriQuery(Request request) {
        List<String> rawQuery = request.getOptions().getUriQueryStrings();
        if (rawQuery == null || rawQuery.isEmpty()) {
            return Map.of();
        }

        Map<String, String> parsed = new LinkedHashMap<>();
        for (String item : rawQuery) {
            if (item == null || item.isBlank()) {
                continue;
            }
            int idx = item.indexOf('=');
            String key = idx >= 0 ? item.substring(0, idx) : item;
            String value = idx >= 0 ? item.substring(idx + 1) : "";
            if (key.isBlank()) {
                continue;
            }
            parsed.put(urlDecode(key), urlDecode(value));
        }
        return parsed;
    }

    private static String urlDecode(String value) {
        return URLDecoder.decode(value, StandardCharsets.UTF_8);
    }

    private int computeServerSecOfYear(Instant now) {
        java.time.ZonedDateTime utc = now.atZone(java.time.ZoneOffset.UTC);
        return (utc.getDayOfYear() - 1) * 24 * 3600
            + utc.getHour() * 3600
            + utc.getMinute() * 60
            + utc.getSecond();
    }

    private static String resolveServerVersion(Map<String, String> registrationQuery, String configuredVersion) {
        if (configuredVersion != null && !configuredVersion.isBlank()) {
            return configuredVersion.trim();
        }
        String clientVersion = trimToEmpty(registrationQuery.get("cv"));
        return clientVersion;
    }

    private SampleConfigResolution loadSampleConfig(String endpoint) {
        if (secretKeyService == null || luciConfigService == null || endpoint.isBlank()) {
            return SampleConfigResolution.defaults("missing-services-or-endpoint");
        }

        String zone = secretKeyService.findManagedZone(endpoint);
        if (zone == null || zone.isBlank()) {
            return SampleConfigResolution.defaults("no-zone");
        }

        Map<String, Object> device = luciConfigService.getDevice(zone, endpoint);
        if (device == null || device.isEmpty()) {
            return SampleConfigResolution.defaults("no-device-config", zone);
        }

        int version = asPositiveInt(device.get("sampling_config_version"), DEFAULT_SAMPLE_VERSION);
        boolean samplingEnabled = asBoolean(device.get("sampling_enabled"), true);
        int samplingRate = asPositiveInt(device.get("sampling_rate"), DEFAULT_SAMPLING_RATE);
        int reportRate = asPositiveInt(device.get("report_rate"), DEFAULT_REPORT_RATE);
        String sleepMode = asNonBlank(device.get("sleep_mode"), DEFAULT_SLEEP_MODE);
        String sensorUartI2c = asNonBlank(device.get("sensor_list_uart_i2c"), "");
        String sensorRs485 = asNonBlank(device.get("sensor_list_rs485"), "");
        String resolution = asNonBlank(device.get("resolution"), DEFAULT_RESOLUTION);
        String serverVersion = asNonBlank(device.get("server_version"), "");

        return new SampleConfigResolution(
            new SampleConfig(
                version,
                samplingEnabled,
                samplingRate,
                reportRate,
                sleepMode,
                DEFAULT_SEND_ACK_TIMEOUT_MS,
                DEFAULT_SEND_RETRY_DELAY_MS,
                DEFAULT_SEND_RETRY_COUNT,
                sensorUartI2c,
                sensorRs485,
                resolution,
                serverVersion),
            "luci-device-config",
            zone);
    }

    private static int asPositiveInt(Object value, int fallback) {
        if (value == null) {
            return fallback;
        }
        try {
            int parsed = Integer.parseInt(String.valueOf(value).trim());
            return parsed > 0 ? parsed : fallback;
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private static boolean asBoolean(Object value, boolean fallback) {
        if (value == null) {
            return fallback;
        }
        if (value instanceof Boolean b) {
            return b;
        }
        String parsed = String.valueOf(value).trim();
        if (parsed.isEmpty()) {
            return fallback;
        }
        return "1".equals(parsed) || "true".equalsIgnoreCase(parsed);
    }

    private static String asNonBlank(Object value, String fallback) {
        if (value == null) {
            return fallback;
        }
        String parsed = String.valueOf(value).trim();
        return parsed.isEmpty() ? fallback : parsed;
    }

    private static String trimToEmpty(String value) {
        return value == null ? "" : value.trim();
    }

    private void scheduleMissingScriptSyncFromQuery(String endpoint, Map<String, String> registrationQuery) {
        if (missingScriptSyncTrigger == null || endpoint.isBlank() || registrationQuery == null || registrationQuery.isEmpty()) {
            return;
        }

        boolean i2cMissing = parseMissingFlag(registrationQuery.get("i2c_missing"));
        boolean rs485Missing = parseMissingFlag(registrationQuery.get("rs485_missing"));
        if (!i2cMissing && !rs485Missing) {
            return;
        }

        // Trigger missing-script sync asynchronously after ACK handling returns,
        // to avoid sending follow-up LwM2M traffic from inside interceptor callback.
        CompletableFuture.runAsync(
                () -> triggerMissingScriptSync(endpoint, i2cMissing, rs485Missing),
                CompletableFuture.delayedExecutor(DEFAULT_MISSING_SYNC_DELAY_MS, TimeUnit.MILLISECONDS));
    }

    private void triggerMissingScriptSync(String endpoint, boolean i2cMissing, boolean rs485Missing) {
        try {
            missingScriptSyncTrigger.trigger(endpoint, i2cMissing, rs485Missing);
        } catch (Exception e) {
            log.warn("registration-ack missing-script trigger failed for endpoint={} i2c_missing={} rs485_missing={}",
                    endpoint,
                    i2cMissing,
                    rs485Missing,
                    e);
        }
    }

    private static boolean parseMissingFlag(String value) {
        if (value == null) {
            return false;
        }
        String normalized = value.trim();
        return "1".equals(normalized) || "true".equalsIgnoreCase(normalized);
    }

    @FunctionalInterface
    public interface MissingScriptSyncTrigger {
        void trigger(String endpoint, boolean i2cMissing, boolean rs485Missing);
    }

    private record SampleConfig(int version,
                                boolean samplingEnabled,
                                int samplingRate,
                                int reportRate,
                                String sleepMode,
                                int sendAckTimeoutMs,
                                int sendRetryDelayMs,
                                int sendRetryCount,
                                String sensorUartI2c,
                                String sensorRs485,
                                String resolution,
                                String serverVersion) {

        private static SampleConfig defaults() {
            return new SampleConfig(
                    DEFAULT_SAMPLE_VERSION,
                    true,
                    DEFAULT_SAMPLING_RATE,
                    DEFAULT_REPORT_RATE,
                    DEFAULT_SLEEP_MODE,
                    DEFAULT_SEND_ACK_TIMEOUT_MS,
                    DEFAULT_SEND_RETRY_DELAY_MS,
                    DEFAULT_SEND_RETRY_COUNT,
                    "",
                    "",
                    DEFAULT_RESOLUTION,
                    "");
        }
    }

    private record SampleConfigResolution(SampleConfig sampleConfig,
                                          String source,
                                          String zone) {
        private static SampleConfigResolution defaults(String source) {
            return new SampleConfigResolution(SampleConfig.defaults(), source, "");
        }

        private static SampleConfigResolution defaults(String source, String zone) {
            return new SampleConfigResolution(SampleConfig.defaults(), source, zone == null ? "" : zone);
        }
    }
}
