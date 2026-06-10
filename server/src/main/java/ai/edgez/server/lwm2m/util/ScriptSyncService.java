package ai.edgez.server.lwm2m.util;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Base64;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import org.eclipse.leshan.core.request.ContentFormat;
import org.eclipse.leshan.core.request.WriteRequest;
import org.eclipse.leshan.core.node.LwM2mResource;
import org.eclipse.leshan.core.node.LwM2mSingleResource;
import org.eclipse.leshan.core.response.WriteResponse;
import org.eclipse.leshan.server.LeshanServer;
import org.eclipse.leshan.server.registration.Registration;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.stereotype.Service;

import com.fasterxml.jackson.core.type.TypeReference;
import com.fasterxml.jackson.databind.ObjectMapper;

@Service
public class ScriptSyncService {

    private static final Logger LOG = LoggerFactory.getLogger(ScriptSyncService.class);
    private static final int SCRIPT_OBJECT_ID = 31025;
    private static final int RES_SCRIPT_ID = 0;
    private static final int RES_SCRIPT_CHUNK = 3;
    private static final int RES_SCRIPT_NAME = 4;
    private static final int RES_SCRIPT_VERSION = 11;
    private static final int SCRIPT_NAME_MAX_LEN = 64;
    private static final TypeReference<Map<String, Object>> MAP_TYPE = new TypeReference<>() {
    };

    private final LuciConfigService luciConfigService;
    private final SecretKeyService secretKeyService;
    private final ObjectMapper objectMapper;

    @Autowired
    public ScriptSyncService(LuciConfigService luciConfigService,
                             SecretKeyService secretKeyService,
                             ObjectMapper objectMapper) {
        this.luciConfigService = luciConfigService;
        this.secretKeyService = secretKeyService;
        this.objectMapper = objectMapper;
    }

    public SyncResult syncMissingFromRegistration(LeshanServer server, Registration registration) {
        if (registration == null || registration.getEndpoint() == null || registration.getEndpoint().isBlank()) {
            return SyncResult.skipped("missing endpoint");
        }

        Map<String, String> attrs = registration.getAdditionalRegistrationAttributes();
        boolean i2cMissing = parseMissingFlag(attrs, "i2c_missing");
        boolean rs485Missing = parseMissingFlag(attrs, "rs485_missing");

        if (!i2cMissing && !rs485Missing) {
            return SyncResult.skipped("no missing script flags in registration attributes");
        }

        return syncTargetGroups(server, registration.getEndpoint(), i2cMissing, rs485Missing);
    }

    public SyncResult syncTargetGroups(LeshanServer server, String endpoint, boolean includeI2c, boolean includeRs485) {
        if (server == null) {
            return SyncResult.failure("Leshan server not available");
        }
        if (endpoint == null || endpoint.isBlank()) {
            return SyncResult.failure("missing endpoint");
        }

        String zone = secretKeyService.findManagedZone(endpoint);
        if (zone == null || zone.isBlank()) {
            return SyncResult.failure("no managed zone for endpoint " + endpoint);
        }

        Registration registration = getRegistration(server, endpoint);
        if (registration == null) {
            return SyncResult.failure("client not currently registered: " + endpoint);
        }

        Map<String, Object> device = luciConfigService.getDevice(zone, endpoint);
        if (device == null) {
            return SyncResult.failure("device config not found for endpoint " + endpoint + " in zone " + zone);
        }

        List<ScriptTarget> targets = collectTargets(zone, device, includeI2c, includeRs485);
        if (targets.isEmpty()) {
            return SyncResult.skipped("no script targets resolved from sensor config");
        }

        List<Map<String, Object>> details = new ArrayList<>();
        int successCount = 0;
        for (ScriptTarget target : targets) {
            Map<String, Object> row = new LinkedHashMap<>();
            row.put("group", target.group());
            row.put("sensor", target.sensorSelection());
            row.put("script_id", target.scriptId());
            row.put("script_version", target.scriptVersion());

            try {
                ScriptPushResult pushResult = createOrUpdateScript(server, registration, target);
                row.put("created", pushResult.created());
                row.put("create_code", pushResult.createCode());
                row.put("write_code", pushResult.writeCode());
                row.put("ok", pushResult.ok());
                if (pushResult.ok()) {
                    successCount++;
                }
            } catch (Exception e) {
                row.put("ok", false);
                row.put("error", e.getMessage());
            }

            details.add(row);
        }

        if (successCount == 0) {
            return SyncResult.failure("failed to push all scripts", details);
        }

        if (successCount < targets.size()) {
            return SyncResult.partial("pushed " + successCount + " of " + targets.size() + " scripts", details);
        }

        return SyncResult.success("script sync complete", details);
    }

    public SyncResult syncSingleScript(LeshanServer server,
                                       String endpoint,
                                       int scriptId,
                                       int scriptVersion,
                                       String scriptName,
                                       String scriptBody) {
        if (server == null) {
            return SyncResult.failure("Leshan server not available");
        }
        if (endpoint == null || endpoint.isBlank()) {
            return SyncResult.failure("missing endpoint");
        }
        if (scriptId <= 0) {
            return SyncResult.failure("script_id must be > 0");
        }
        if (scriptVersion <= 0) {
            return SyncResult.failure("script_version must be > 0");
        }
        if (scriptBody == null || scriptBody.isBlank()) {
            return SyncResult.failure("missing raw script body");
        }

        Registration registration = getRegistration(server, endpoint);
        if (registration == null) {
            return SyncResult.failure("client not currently registered: " + endpoint);
        }

        ScriptTarget target = new ScriptTarget("manual",
                                               scriptName == null || scriptName.isBlank() ? String.valueOf(scriptId) : scriptName,
                                               scriptId,
                                               scriptVersion,
                                               scriptName == null ? "" : scriptName,
                                               scriptBody);
        try {
            ScriptPushResult pushResult = createOrUpdateScript(server, registration, target);
            Map<String, Object> detail = new LinkedHashMap<>();
            detail.put("script_id", scriptId);
            detail.put("script_version", scriptVersion);
            detail.put("create_code", pushResult.createCode());
            detail.put("write_code", pushResult.writeCode());
            detail.put("ok", pushResult.ok());
            if (!pushResult.ok()) {
                return SyncResult.failure("script push failed", List.of(detail));
            }
            return SyncResult.success("script pushed", List.of(detail));
        } catch (Exception e) {
            return SyncResult.failure("script push failed: " + e.getMessage());
        }
    }

    private Registration getRegistration(LeshanServer server, String endpoint) {
        return server.getRegistrationService().getByEndpoint(endpoint);
    }

    private List<ScriptTarget> collectTargets(String zone,
                                              Map<String, Object> device,
                                              boolean includeI2c,
                                              boolean includeRs485) {
        List<ScriptTarget> targets = new ArrayList<>();
        if (includeI2c) {
            String selection = asString(device.get("sensor_list_uart_i2c"));
            targets.addAll(resolveTargetsForGroup(zone, "uart_i2c", selection));
        }
        if (includeRs485) {
            String selection = asString(device.get("sensor_list_rs485"));
            targets.addAll(resolveTargetsForGroup(zone, "rs485", selection));
        }

        LinkedHashMap<Integer, ScriptTarget> dedup = new LinkedHashMap<>();
        for (ScriptTarget target : targets) {
            dedup.putIfAbsent(target.scriptId(), target);
        }
        return new ArrayList<>(dedup.values());
    }

    private List<ScriptTarget> resolveTargetsForGroup(String zone, String group, String selection) {
        List<ScriptTarget> out = new ArrayList<>();
        if (selection == null || selection.isBlank() || "none".equalsIgnoreCase(selection) || "off".equalsIgnoreCase(selection)) {
            return out;
        }

        List<Map<String, Object>> rows = luciConfigService.listSensors(zone, group);
        if (rows.isEmpty()) {
            return out;
        }

        for (String token : splitSelections(selection)) {
            LuciConfigService.SensorIdentity sensorIdentity = luciConfigService.parseSensorIdentity(token, "");
            String combinedId = luciConfigService.formatCombinedSensorId(sensorIdentity.id(), sensorIdentity.version());

            Map<String, Object> matched = null;
            for (Map<String, Object> row : rows) {
                String rowId = asString(row.get("id"));
                if (combinedId.equalsIgnoreCase(rowId)) {
                    matched = row;
                    break;
                }
            }

            if (matched == null) {
                continue;
            }

            String driverJson = asString(matched.get("driver_json"));
            if (driverJson.isBlank()) {
                continue;
            }

            try {
                Map<String, Object> parsed = objectMapper.readValue(driverJson, MAP_TYPE);
                int scriptId = parsePositiveInt(parsed.get("id"));
                if (scriptId <= 0) {
                    continue;
                }

                String scriptPayload = parseScriptPayload(parsed.get("script"));
                if (scriptPayload.isBlank()) {
                    continue;
                }

                int scriptVersion = parsePositiveInt(sensorIdentity.version());
                if (scriptVersion <= 0) {
                    scriptVersion = 1;
                }

                String scriptName = asString(matched.get("name"));
                if (scriptName.isBlank()) {
                    scriptName = combinedId;
                }

                out.add(new ScriptTarget(group,
                                         combinedId,
                                         scriptId,
                                         scriptVersion,
                                         scriptName,
                                         scriptPayload));
            } catch (Exception e) {
                LOG.warn("Failed to parse driver_json for zone={}, group={}, selection={}", zone, group, token, e);
            }
        }

        return out;
    }

    private ScriptPushResult createOrUpdateScript(LeshanServer server, Registration registration, ScriptTarget target)
            throws InterruptedException {
        LOG.info("Handling script sync push endpoint={} scriptId={} scriptVersion={}",
            registration == null ? "" : registration.getEndpoint(),
            target.scriptId(),
            target.scriptVersion());

        Collection<LwM2mResource> replaceResources = new ArrayList<>();
        replaceResources.add(LwM2mSingleResource.newIntegerResource(RES_SCRIPT_VERSION, target.scriptVersion()));
        String deviceScriptName = sanitizeScriptNameForDevice(target.scriptName());
        if (!deviceScriptName.isBlank()) {
            replaceResources.add(LwM2mSingleResource.newStringResource(RES_SCRIPT_NAME, deviceScriptName));
        }
        replaceResources.add(LwM2mSingleResource.newBinaryResource(RES_SCRIPT_CHUNK,
            target.scriptPayload().getBytes(StandardCharsets.UTF_8)));

        // Use a single REPLACE instance write to match the device-side expected write flow.
        WriteRequest replaceRequest = new WriteRequest(
            WriteRequest.Mode.REPLACE,
            ContentFormat.TLV,
            SCRIPT_OBJECT_ID,
            target.scriptId(),
            replaceResources);
        WriteResponse replaceResponse = server.send(registration, replaceRequest, 5000L);
        return new ScriptPushResult(
                false,
                "SKIPPED",
                replaceResponse != null ? String.valueOf(replaceResponse.getCode()) : "(null)",
                replaceResponse != null && replaceResponse.isSuccess());
    }

    private static String sanitizeScriptNameForDevice(String rawName) {
        if (rawName == null || rawName.isBlank()) {
            return "";
        }

        StringBuilder out = new StringBuilder();
        String trimmed = rawName.trim();
        for (int i = 0; i < trimmed.length(); i++) {
            char c = trimmed.charAt(i);
            boolean allowed = Character.isLetterOrDigit(c) || c == '_' || c == '-' || c == '.' || c == ':';
            if (allowed) {
                out.append(c);
            } else if (Character.isWhitespace(c)) {
                out.append('_');
            }

            if (out.length() >= (SCRIPT_NAME_MAX_LEN - 1)) {
                break;
            }
        }

        return out.toString();
    }

    private static List<String> splitSelections(String value) {
        String[] parts = value.split("[\\s,;]+");
        List<String> out = new ArrayList<>();
        for (String part : parts) {
            String token = part == null ? "" : part.trim();
            if (!token.isBlank()) {
                out.add(token);
            }
        }
        return out;
    }

    private static boolean parseMissingFlag(Map<String, String> attrs, String key) {
        if (attrs == null) {
            return false;
        }
        String value = attrs.get(key);
        if (value == null) {
            return false;
        }
        String normalized = value.trim();
        return "1".equals(normalized) || "true".equalsIgnoreCase(normalized);
    }

    private static int parsePositiveInt(Object value) {
        if (value == null) {
            return -1;
        }
        try {
            int out = Integer.parseInt(String.valueOf(value).trim());
            return out > 0 ? out : -1;
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    private static String parseScriptPayload(Object raw) {
        String payload = asString(raw);
        if (payload.isBlank()) {
            return "";
        }

        // Mirror wakaama's permissive behavior: if payload looks like base64, decode it.
        try {
            byte[] decoded = Base64.getDecoder().decode(payload);
            if (decoded.length > 0) {
                return new String(decoded, StandardCharsets.UTF_8);
            }
        } catch (IllegalArgumentException ignored) {
            // Not base64, use raw payload.
        }

        return payload;
    }

    private static String asString(Object value) {
        return value == null ? "" : String.valueOf(value).trim();
    }

    private record ScriptTarget(String group,
                                String sensorSelection,
                                int scriptId,
                                int scriptVersion,
                                String scriptName,
                                String scriptPayload) {
    }

    private record ScriptPushResult(boolean created,
                                    String createCode,
                                    String writeCode,
                                    boolean ok) {
    }

    public record SyncResult(boolean success,
                             boolean partial,
                             boolean skipped,
                             String message,
                             List<Map<String, Object>> details) {
        public static SyncResult success(String message, List<Map<String, Object>> details) {
            return new SyncResult(true, false, false, message, details == null ? List.of() : details);
        }

        public static SyncResult partial(String message, List<Map<String, Object>> details) {
            return new SyncResult(false, true, false, message, details == null ? List.of() : details);
        }

        public static SyncResult skipped(String message) {
            return new SyncResult(false, false, true, message, List.of());
        }

        public static SyncResult failure(String message) {
            return new SyncResult(false, false, false, message, List.of());
        }

        public static SyncResult failure(String message, List<Map<String, Object>> details) {
            return new SyncResult(false, false, false, message, details == null ? List.of() : details);
        }

        public Map<String, Object> toResponseBody() {
            Map<String, Object> body = new LinkedHashMap<>();
            body.put("success", success);
            body.put("partial", partial);
            body.put("skipped", skipped);
            body.put("message", message);
            body.put("details", details == null ? List.of() : details);
            return body;
        }
    }
}
