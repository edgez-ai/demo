package ai.edgez.server.lwm2m.config;

import java.nio.charset.StandardCharsets;

import org.eclipse.californium.core.coap.CoAP;
import org.eclipse.californium.core.coap.EmptyMessage;
import org.eclipse.californium.core.coap.Request;
import org.eclipse.californium.core.coap.Response;
import org.eclipse.californium.core.network.interceptors.MessageInterceptor;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

/**
 * Logs incoming LwM2M Send SenML JSON reports on /dp and normalizes known
 * compatibility issues before Leshan decodes the payload.
 */
public class SendPayloadCompatibilityInterceptor implements MessageInterceptor {

    private static final Logger log = LoggerFactory.getLogger(SendPayloadCompatibilityInterceptor.class);

    // LwM2M SenML JSON content format (application/senml+json)
    private static final int CONTENT_FORMAT_SENML_JSON = 110;
    private final ObjectMapper mapper = new ObjectMapper();

    @Override
    public void receiveRequest(Request request) {
        if (request == null || !isSendDataPush(request)) {
            return;
        }

        byte[] payload = request.getPayload();
        if (payload == null || payload.length == 0) {
            return;
        }

        String json = new String(payload, StandardCharsets.UTF_8);
        log.info("Sample report received from {}: {}",
                request.getSourceContext() != null ? request.getSourceContext().getPeerAddress() : "unknown",
                json);

        // For large blockwise uploads, each receiveRequest call may carry only one
        // fragment (not valid JSON by itself). Never mutate fragment length here,
        // otherwise Californium block position tracking breaks (4.08).
        if (request.getOptions() != null && request.getOptions().hasBlock1()) {
            log.debug("Skipping /dp normalization for blockwise fragment");
            return;
        }

        try {
            JsonNode root = mapper.readTree(json);
            if (!(root instanceof ArrayNode)) {
                return;
            }

            boolean changed = false;
            ArrayNode records = (ArrayNode) root;
            for (JsonNode record : records) {
                if (!(record instanceof ObjectNode)) {
                    continue;
                }

                ObjectNode obj = (ObjectNode) record;
                JsonNode vd = obj.get("vd");
                if (vd == null || !vd.isTextual()) {
                    continue;
                }

                String rawVd = vd.asText();
                String normalizedVd = stripTrailingPadding(rawVd);
                if (!normalizedVd.equals(rawVd)) {
                    obj.put("vd", normalizedVd);
                    changed = true;
                }
            }

            if (changed) {
                String normalizedJson = mapper.writeValueAsString(records);
                request.setPayload(normalizedJson.getBytes(StandardCharsets.UTF_8));
                log.info("Sample report normalized for Leshan decoder compatibility (vd padding removed)");
            }
        } catch (JsonProcessingException e) {
            log.debug("Unable to parse /dp SenML payload for normalization: {}", e.getMessage());
        }
    }

    @Override
    public void sendResponse(Response response) {
        // no-op
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

    private static boolean isSendDataPush(Request request) {
        if (request.getCode() != CoAP.Code.POST) {
            return false;
        }

        if (request.getOptions() == null) {
            return false;
        }

        String path = request.getOptions().getUriPathString();
        if (path == null) {
            return false;
        }

        String normalizedPath = path.startsWith("/") ? path.substring(1) : path;
        if (!"dp".equals(normalizedPath)) {
            return false;
        }

        int contentFormat = request.getOptions().getContentFormat();
        return contentFormat == CONTENT_FORMAT_SENML_JSON;
    }

    private static String stripTrailingPadding(String input) {
        int end = input.length();
        while (end > 0 && input.charAt(end - 1) == '=') {
            end--;
        }
        return end == input.length() ? input : input.substring(0, end);
    }

}
