package ai.edgez.server.lwm2m.config;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.eclipse.californium.core.coap.CoAP;
import org.eclipse.californium.core.coap.Request;
import org.eclipse.californium.core.coap.Response;
import org.junit.jupiter.api.Test;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

class RegistrationAckPayloadInterceptorTest {

    private final ObjectMapper mapper = new ObjectMapper();

    @Test
    void shouldInjectRegistrationAckPayloadWithClientArgs() throws Exception {
        RegistrationAckPayloadInterceptor interceptor = new RegistrationAckPayloadInterceptor(
                true,
                true,
                300,
                null,
            null,
            null);

        Request registration = Request.newPost();
        registration.getOptions().setUriPath("rd");
        registration.getOptions().addUriQuery("ep=edge-001");
        registration.getOptions().addUriQuery("sv=42");
        registration.getOptions().addUriQuery("cv=fw-1.2.3");
        registration.setToken(new byte[] { 0x01, 0x23, 0x45 });
        interceptor.receiveRequest(registration);

        Response ack = new Response(CoAP.ResponseCode.CREATED);
        ack.setToken(new byte[] { 0x01, 0x23, 0x45 });
        ack.getOptions().addLocationPath("rd");
        ack.getOptions().addLocationPath("123");

        interceptor.sendResponse(ack);

        JsonNode payload = mapper.readTree(ack.getPayloadString());
        assertEquals("registration-ack", payload.get("type").asText());
        assertEquals("edge-001", payload.get("endpoint").asText());
        assertEquals("42", payload.get("registrationParams").get("sv").asText());
        assertEquals("fw-1.2.3", payload.get("registrationParams").get("cv").asText());

        // Default config is used when no per-device config services are provided.
        assertEquals(1, payload.get("version").asInt());
        assertTrue(payload.get("sampling_enabled").asBoolean());
        assertEquals(60, payload.get("sampling_rate").asInt());
        assertEquals(300, payload.get("report_rate").asInt());
        assertTrue(payload.has("server_uptime_ms"));
    }

    @Test
    void shouldNotInjectPayloadForNonRegistrationResponse() {
        RegistrationAckPayloadInterceptor interceptor = new RegistrationAckPayloadInterceptor(
                true,
                true,
                300,
                null,
            null,
            null);

        Response nonRegistrationAck = new Response(CoAP.ResponseCode.CHANGED);
        nonRegistrationAck.getOptions().addLocationPath("rd");
        nonRegistrationAck.setToken(new byte[] { 0x01 });

        interceptor.sendResponse(nonRegistrationAck);

        assertTrue(nonRegistrationAck.getPayload() == null || nonRegistrationAck.getPayloadSize() == 0);
    }
}
