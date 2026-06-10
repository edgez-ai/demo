package ai.edgez.server.lwm2m.config;

import java.io.IOException;
import java.util.Collection;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

import org.eclipse.californium.core.CoapServer;
import org.eclipse.californium.core.config.CoapConfig;
import org.eclipse.californium.core.network.Endpoint;
import org.eclipse.californium.elements.config.Configuration;
import org.eclipse.californium.core.network.CoapEndpoint;
import org.eclipse.californium.core.server.MessageDeliverer;
import org.eclipse.californium.scandium.config.DtlsConfig;
import org.eclipse.californium.scandium.dtls.InMemorySessionStore;
import org.eclipse.californium.scandium.dtls.MaxFragmentLengthExtension.Length;
import org.eclipse.leshan.core.model.InvalidDDFFileException;
import org.eclipse.leshan.core.model.InvalidModelException;
import org.eclipse.leshan.core.observation.CompositeObservation;
import org.eclipse.leshan.core.observation.Observation;
import org.eclipse.leshan.core.observation.SingleObservation;
import org.eclipse.leshan.core.response.ObserveCompositeResponse;
import org.eclipse.leshan.core.response.ObserveResponse;
import org.eclipse.leshan.server.LeshanServer;
import org.eclipse.leshan.server.LeshanServerBuilder;
import org.eclipse.leshan.server.model.LwM2mModelProvider;
import org.eclipse.leshan.server.observation.ObservationListener;
import org.eclipse.leshan.server.registration.Registration;
import org.eclipse.leshan.server.registration.RegistrationListener;
import org.eclipse.leshan.server.registration.RegistrationStore;
import org.eclipse.leshan.server.registration.RegistrationUpdate;
import org.eclipse.leshan.servers.security.SecurityStore;
import org.eclipse.leshan.transport.californium.server.endpoint.CaliforniumServerEndpointsProvider;
import org.eclipse.leshan.transport.californium.server.endpoint.coap.CoapServerProtocolProvider;
import org.eclipse.leshan.transport.californium.server.endpoint.coaps.CoapsServerProtocolProvider;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

import ai.edgez.server.lwm2m.handler.CustomSendHandler;
import ai.edgez.server.lwm2m.util.LuciConfigService;
import ai.edgez.server.lwm2m.util.SecretKeyService;
import ai.edgez.server.lwm2m.util.ScriptSyncService;
import ai.edgez.server.lwm2m.util.SampleReportMongoService;
import ai.edgez.server.lwm2m.wasm.WasmRuntimeService;
import ai.edgez.server.lwm2m.wasm.LeshanWasmLwm2mHost;
import jakarta.annotation.PostConstruct;
import jakarta.annotation.PreDestroy;

@Component
public class LeshanServerComponent {
    private static final Logger log = LoggerFactory.getLogger(LeshanServerComponent.class);

    private LeshanServer server;
    private CustomSendHandler customSendHandler;
    private final ExecutorService missingScriptSyncExecutor = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "missing-script-sync");
        t.setDaemon(true);
        return t;
    });
    private final ConcurrentMap<String, PendingMissingFlags> pendingMissingFlagsByEndpoint = new ConcurrentHashMap<>();
    private final ConcurrentMap<String, Boolean> missingSyncInFlightByEndpoint = new ConcurrentHashMap<>();

    @Value("${lwm2m.coap-port:5683}")
    private int coapPort;

    @Value("${lwm2m.coaps-port:5684}")
    private int coapsPort;

    @Value("${lwm2m.registration.ack.enabled:true}")
    private boolean registrationAckPayloadEnabled;

    @Value("${lwm2m.registration.ack.include-query-params:true}")
    private boolean registrationAckIncludeQueryParams;

    @Value("${lwm2m.registration.ack.time-sync-interval-sec:300}")
    private int registrationAckTimeSyncIntervalSec;
    
    @Autowired
    private SecurityStore securityStore;
    
    @Autowired
    private RegistrationStore registrationStore;

    @Autowired
    private LwM2mModelProvider modelProvider;

    @Autowired
    private SecretKeyService secretKeyService;

    @Autowired
    private LuciConfigService luciConfigService;

    @Autowired
    private ScriptSyncService scriptSyncService;

    @Autowired(required = false)
    private SampleReportMongoService sampleReportMongoService;

    //@Autowired
    //private WasmRuntimeService wasmRuntimeService;


    @PostConstruct
    public void startServer() throws IOException, InvalidModelException, InvalidDDFFileException {
        // Create CustomSendHandler directly to avoid circular dependency
        try {
            customSendHandler = new CustomSendHandler();
        } catch (Exception e) {
            throw new RuntimeException("Failed to create CustomSendHandler", e);
        }

        LeshanServerBuilder builder = new LeshanServerBuilder();


        builder.setObjectModelProvider(modelProvider);
        
        builder.setSecurityStore(securityStore);
        builder.setRegistrationStore(registrationStore);

        // Define trust store
 //       List<Certificate> trustStore = cli.identity.getTrustStore();
 //       builder.setTrustedCertificates(trustStore.toArray(new Certificate[trustStore.size()]));

        // Configure Californium endpoints (CoAP and CoAPS)
        CaliforniumServerEndpointsProvider.Builder endpointsBuilder = new CaliforniumServerEndpointsProvider.Builder(
                new CoapServerProtocolProvider(),
                new CoapsServerProtocolProvider(c -> {
                    // Add MDC for connection logs
                        c.setSessionStore(new InMemorySessionStore(1000,300));

                }));

        // Create default configuration
        Configuration serverCoapConfig = endpointsBuilder.createDefaultConfiguration();
        // 10 minutes idle allowed
        serverCoapConfig.set(DtlsConfig.DTLS_STALE_CONNECTION_THRESHOLD, 300,TimeUnit.SECONDS);
        serverCoapConfig.set(DtlsConfig.DTLS_AUTO_HANDSHAKE_TIMEOUT, 300*1000, TimeUnit.MILLISECONDS);
        // Keep DTLS records small enough to avoid UDP truncation on constrained links
        serverCoapConfig.set(DtlsConfig.DTLS_MAX_TRANSMISSION_UNIT, 1200);
        serverCoapConfig.set(DtlsConfig.DTLS_MAX_TRANSMISSION_UNIT_LIMIT, 1200);
        serverCoapConfig.set(DtlsConfig.DTLS_MAX_FRAGMENT_LENGTH, Length.BYTES_1024);
        
        // Increase buffer size for Block1-wise transfers (default is 8192 bytes)
        // Set to 2MB to allow large image uploads via blockwise transfer
        serverCoapConfig.set(CoapConfig.MAX_RESOURCE_BODY_SIZE, 2 * 1024 * 1024); // 2MB

        // Apply externally-configured ports (defaults 5683/5684)
        serverCoapConfig.set(CoapConfig.COAP_PORT, coapPort);
        serverCoapConfig.set(CoapConfig.COAP_SECURE_PORT, coapsPort);
        
        // Apply configuration before adding endpoints
        endpointsBuilder.setConfiguration(serverCoapConfig);
        
        // Add both CoAP (non-secure) and CoAPS (secure) endpoints
        endpointsBuilder.addEndpoint("coap://0.0.0.0:" + coapPort);
        endpointsBuilder.addEndpoint("coaps://0.0.0.0:" + coapsPort);

        builder.setEndpointsProviders(endpointsBuilder.build());

    server = builder.build();

    installRegistrationAckPayloadInterceptor();
    installSendPayloadCompatibilityInterceptor();
    installSendPayloadPostBlockwiseNormalizer();

    // Provide LwM2M host operations to wasm imports.
    // wasmRuntimeService.setLwm2mHost(new LeshanWasmLwm2mHost(server));
    
    // Inject the server instance into CustomSendHandler
    customSendHandler.setLeshanServer(server);
    customSendHandler.setSampleReportMongoService(sampleReportMongoService);
    customSendHandler.setSecretKeyService(secretKeyService);
    customSendHandler.setLuciConfigService(luciConfigService);
    
    server.getSendService().addListener(customSendHandler);
        server.getRegistrationService().addListener(new RegistrationListener() {
            @Override
            public void registered(Registration reg, Registration previousReg, Collection<Observation> previousObsersations) {
                        ensureDeviceRecordFromRegistration(reg);
                        triggerMissingScriptSync(reg);
                        // wasmRuntimeService.handleEndpointRegistered(reg.getEndpoint(), toObjectLinksJson(reg));
            }

            @Override
            public void updated(RegistrationUpdate update, Registration updatedReg, Registration previousReg) {
                Registration target = updatedReg != null ? updatedReg : previousReg;
                if (target != null) {
                        ensureDeviceRecordFromRegistration(target);
                    triggerMissingScriptSync(target);
                        // wasmRuntimeService.handleEndpointRegistered(target.getEndpoint(), toObjectLinksJson(target));
                }
            }

            @Override
            public void unregistered(Registration registration, Collection<Observation> observations, boolean expired,
                    Registration newReg) {
                if (registration != null) {
                        // wasmRuntimeService.handleEndpointDeregistered(registration.getEndpoint());
                }
            }
        });
        // Listen for notifications from observed resources
        server.getObservationService().addListener(new ObservationListener() {

            @Override
            public void newObservation(Observation observation, Registration registration) {
                System.out.println("New observation started for " + observation.getRegistrationId());
            }

            @Override
            public void cancelled(Observation observation) {
                System.out.println("Observation cancelled for " + observation.getRegistrationId());
            }

            @Override
            public void onResponse(SingleObservation observation, Registration registration, ObserveResponse response) {
                if (response.isSuccess()) {
                    System.out.println("Notification received for " + observation.getRegistrationId() + ": " + response.getContent());
                    if (registration != null) {
                        //  wasmRuntimeService.callEvent(registration.getEndpoint());
                    }
                } else {
                    System.err.println("Failed notification for " + observation.getRegistrationId() + ": " + response.getCode());
                }
            }

            @Override
            public void onResponse(CompositeObservation observation, Registration registration,
                    ObserveCompositeResponse response) {
                System.out.println("Composite notification received for " + observation.getRegistrationId() + ": " + response);
            }

            @Override
            public void onError(Observation observation, Registration registration, Exception error) {
                System.err.println("Observation error for " + observation.getRegistrationId() + ": " + error.getMessage());
            }

        });
        try {
            server.start();
            log.info("Leshan server started on coap://0.0.0.0:{} and coaps://0.0.0.0:{}", coapPort, coapsPort);
        } catch (Exception e) {
            log.error("Failed to start Leshan server on coap:{} / coaps:{}", coapPort, coapsPort, e);
            throw e;
        }
    }

    @PreDestroy
    public void stopServer() {
        if (server != null) {
            server.stop();
            System.out.println("Leshan server stopped.");
        }
        missingScriptSyncExecutor.shutdownNow();
    }

    public LeshanServer getServer() {
        return server;
    }

    private void installRegistrationAckPayloadInterceptor() {
        if (!registrationAckPayloadEnabled || server == null) {
            return;
        }

        RegistrationAckPayloadInterceptor interceptor = new RegistrationAckPayloadInterceptor(
                true,
                registrationAckIncludeQueryParams,
            registrationAckTimeSyncIntervalSec,
            secretKeyService,
            luciConfigService,
            this::triggerMissingScriptSync);

        int attached = 0;
        for (org.eclipse.leshan.server.endpoint.LwM2mServerEndpoint endpoint : server.getEndpoints()) {
            if (endpoint instanceof org.eclipse.leshan.transport.californium.server.endpoint.CaliforniumServerEndpoint) {
                CoapEndpoint coapEndpoint = ((org.eclipse.leshan.transport.californium.server.endpoint.CaliforniumServerEndpoint) endpoint)
                        .getCoapEndpoint();
                coapEndpoint.addInterceptor(interceptor);
                attached++;
            }
        }

        if (attached == 0) {
            for (org.eclipse.leshan.server.endpoint.LwM2mServerEndpointsProvider provider : server.getEndpointsProvider()) {
                if (provider instanceof CaliforniumServerEndpointsProvider) {
                    for (Endpoint endpoint : ((CaliforniumServerEndpointsProvider) provider).getCoapServer().getEndpoints()) {
                        if (endpoint instanceof CoapEndpoint) {
                            ((CoapEndpoint) endpoint).addInterceptor(interceptor);
                            attached++;
                        }
                    }
                }
            }
        }

        log.info("Registration ACK payload interceptor enabled on {} endpoint(s)", attached);
    }

    private void installSendPayloadCompatibilityInterceptor() {
        if (server == null) {
            return;
        }

        SendPayloadCompatibilityInterceptor interceptor = new SendPayloadCompatibilityInterceptor();

        int attached = 0;
        for (org.eclipse.leshan.server.endpoint.LwM2mServerEndpoint endpoint : server.getEndpoints()) {
            if (endpoint instanceof org.eclipse.leshan.transport.californium.server.endpoint.CaliforniumServerEndpoint) {
                CoapEndpoint coapEndpoint = ((org.eclipse.leshan.transport.californium.server.endpoint.CaliforniumServerEndpoint) endpoint)
                        .getCoapEndpoint();
                coapEndpoint.addInterceptor(interceptor);
                attached++;
            }
        }

        if (attached == 0) {
            for (org.eclipse.leshan.server.endpoint.LwM2mServerEndpointsProvider provider : server.getEndpointsProvider()) {
                if (provider instanceof CaliforniumServerEndpointsProvider) {
                    for (Endpoint endpoint : ((CaliforniumServerEndpointsProvider) provider).getCoapServer().getEndpoints()) {
                        if (endpoint instanceof CoapEndpoint) {
                            ((CoapEndpoint) endpoint).addInterceptor(interceptor);
                            attached++;
                        }
                    }
                }
            }
        }

        log.info("Send payload compatibility interceptor enabled on {} endpoint(s)", attached);
    }

    private void installSendPayloadPostBlockwiseNormalizer() {
        if (server == null) {
            return;
        }

        int attached = 0;
        for (org.eclipse.leshan.server.endpoint.LwM2mServerEndpointsProvider provider : server.getEndpointsProvider()) {
            if (!(provider instanceof CaliforniumServerEndpointsProvider)) {
                continue;
            }

            CoapServer coapServer = ((CaliforniumServerEndpointsProvider) provider).getCoapServer();
            if (coapServer == null) {
                continue;
            }

            MessageDeliverer existing = coapServer.getMessageDeliverer();
            if (existing instanceof SendPayloadPostBlockwiseNormalizer) {
                continue;
            }

            coapServer.setMessageDeliverer(new SendPayloadPostBlockwiseNormalizer(existing));
            attached++;
        }

        log.info("Send payload post-blockwise normalizer enabled on {} provider(s)", attached);
    }

    private static String toObjectLinksJson(Registration reg) {
        if (reg == null || reg.getObjectLinks() == null) {
            return "[]";
        }
        StringBuilder sb = new StringBuilder("[");
        Object[] links = reg.getObjectLinks();
        for (int i = 0; i < links.length; i++) {
            if (i > 0) {
                sb.append(',');
            }
            sb.append('"').append(String.valueOf(links[i])).append('"');
        }
        sb.append(']');
        return sb.toString();
    }

    private void ensureDeviceRecordFromRegistration(Registration registration) {
        if (registration == null) {
            return;
        }

        String endpoint = registration.getEndpoint();
        if (endpoint == null || endpoint.isBlank()) {
            return;
        }

        String zone = secretKeyService.findManagedZone(endpoint);
        if (zone == null || zone.isBlank()) {
            log.debug("Skipping luci_devices upsert: no managed zone for endpoint {}", endpoint);
            return;
        }

        String identity = secretKeyService.findManagedIdentity(zone, endpoint);
        String serial = (identity != null && !identity.isBlank()) ? identity : endpoint;
        luciConfigService.ensureDeviceRecord(zone, endpoint, serial);
    }

    private void triggerMissingScriptSync(Registration registration) {
        if (registration == null || scriptSyncService == null) {
            return;
        }

        Map<String, String> attrs = registration.getAdditionalRegistrationAttributes();
        boolean i2cMissing = parseMissingFlag(attrs, "i2c_missing");
        boolean rs485Missing = parseMissingFlag(attrs, "rs485_missing");
        if (!i2cMissing && !rs485Missing) {
            return;
        }

        scheduleMissingScriptSync(registration.getEndpoint(), i2cMissing, rs485Missing);
    }

    private void triggerMissingScriptSync(String endpoint, boolean i2cMissing, boolean rs485Missing) {
        if (scriptSyncService == null || endpoint == null || endpoint.isBlank()) {
            return;
        }
        if (!i2cMissing && !rs485Missing) {
            return;
        }

        scheduleMissingScriptSync(endpoint, i2cMissing, rs485Missing);
    }

    private void scheduleMissingScriptSync(String endpoint, boolean i2cMissing, boolean rs485Missing) {
        pendingMissingFlagsByEndpoint.merge(
                endpoint,
                new PendingMissingFlags(i2cMissing, rs485Missing),
                PendingMissingFlags::merge);

        if (missingSyncInFlightByEndpoint.putIfAbsent(endpoint, Boolean.TRUE) != null) {
            return;
        }

        CompletableFuture.runAsync(() -> drainMissingScriptSync(endpoint), missingScriptSyncExecutor);
    }

    private void drainMissingScriptSync(String endpoint) {
        try {
            while (true) {
                PendingMissingFlags flags = pendingMissingFlagsByEndpoint.remove(endpoint);
                if (flags == null || (!flags.i2cMissing() && !flags.rs485Missing())) {
                    break;
                }

                runMissingScriptSync(endpoint, flags.i2cMissing(), flags.rs485Missing());
            }
        } finally {
            missingSyncInFlightByEndpoint.remove(endpoint);
            // If new flags arrived while we were draining, restart once.
            if (pendingMissingFlagsByEndpoint.containsKey(endpoint)
                    && missingSyncInFlightByEndpoint.putIfAbsent(endpoint, Boolean.TRUE) == null) {
                CompletableFuture.runAsync(() -> drainMissingScriptSync(endpoint), missingScriptSyncExecutor);
            }
        }
    }

    private void runMissingScriptSync(String endpoint, boolean i2cMissing, boolean rs485Missing) {
        try {
            ScriptSyncService.SyncResult result = scriptSyncService.syncTargetGroups(server, endpoint, i2cMissing, rs485Missing);
            if (result.skipped()) {
                log.debug("Missing-script sync skipped for endpoint {}: i2c_missing={} rs485_missing={} message={}",
                        endpoint,
                        i2cMissing,
                        rs485Missing,
                        result.message());
                return;
            }

            if (result.success() || result.partial()) {
                log.info("Missing-script sync result for endpoint {}: success={} partial={} i2c_missing={} rs485_missing={} message={} details={}",
                        endpoint,
                        result.success(),
                        result.partial(),
                        i2cMissing,
                        rs485Missing,
                        result.message(),
                        result.details());
            } else {
                log.warn("Missing-script sync failed for endpoint {}: i2c_missing={} rs485_missing={} message={} details={}",
                        endpoint,
                        i2cMissing,
                        rs485Missing,
                        result.message(),
                        result.details());
            }
        } catch (Exception e) {
            log.warn("Missing-script sync error for endpoint {}", endpoint, e);
        }
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

    private record PendingMissingFlags(boolean i2cMissing, boolean rs485Missing) {
        private PendingMissingFlags merge(PendingMissingFlags other) {
            if (other == null) {
                return this;
            }
            return new PendingMissingFlags(this.i2cMissing || other.i2cMissing,
                                           this.rs485Missing || other.rs485Missing);
        }
    }
}
