package ai.edgez.server.lwm2m.config;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.stream.Collectors;

import org.eclipse.leshan.core.link.Link;
import org.eclipse.leshan.core.model.InvalidDDFFileException;
import org.eclipse.leshan.core.model.InvalidModelException;
import org.eclipse.leshan.core.model.ObjectLoader;
import org.eclipse.leshan.core.model.ObjectModel;
import org.eclipse.leshan.core.node.LwM2mNode;
import org.eclipse.leshan.core.response.LwM2mResponse;
import org.eclipse.leshan.server.model.LwM2mModelProvider;
import org.eclipse.leshan.server.model.VersionedModelProvider;
import org.eclipse.leshan.server.registration.InMemoryRegistrationStore;
import org.eclipse.leshan.server.registration.Registration;
import org.eclipse.leshan.server.registration.RegistrationStore;
import org.eclipse.leshan.servers.security.EditableSecurityStore;
import org.eclipse.leshan.servers.security.InMemorySecurityStore;
import org.eclipse.leshan.servers.security.NonUniqueSecurityInfoException;
import org.eclipse.leshan.servers.security.SecurityInfo;
import org.eclipse.leshan.servers.security.SecurityStore;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.core.io.Resource;
import org.springframework.core.io.support.PathMatchingResourcePatternResolver;
import org.springframework.core.io.support.ResourcePatternResolver;
import org.springframework.jdbc.core.JdbcTemplate;

import com.fasterxml.jackson.annotation.JsonInclude;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.module.SimpleModule;

import ai.edgez.server.lwm2m.model.JacksonLinkSerializer;
import ai.edgez.server.lwm2m.model.JacksonLwM2mNodeDeserializer;
import ai.edgez.server.lwm2m.model.JacksonLwM2mNodeSerializer;
import ai.edgez.server.lwm2m.model.JacksonRegistrationSerializer;
import ai.edgez.server.lwm2m.model.JacksonResponseSerializer;
import ai.edgez.server.lwm2m.util.SecretKeyService;
@Configuration
public class ServerConfig {

    private static final Logger LOG = LoggerFactory.getLogger(ServerConfig.class);

    @Bean
    public RegistrationStore registrationStore() {
        return new InMemoryRegistrationStore();
    }

    @Bean
    public EditableSecurityStore securityStore(SecretKeyService secretKeyService, JdbcTemplate jdbcTemplate) {
        InMemorySecurityStore store = new InMemorySecurityStore();
        try {
            store.add(SecurityInfo.newPreSharedKeyInfo("edge-device", "edge-device-psk-id",
                    "edge-device-psk-key".getBytes(StandardCharsets.UTF_8)));

            List<Map<String, Object>> managedRows = jdbcTemplate.queryForList(
                    "SELECT endpoint, identity FROM managed_psk ORDER BY updated_at DESC");
            for (Map<String, Object> row : managedRows) {
                String endpoint = row.get("ENDPOINT") == null ? null : String.valueOf(row.get("ENDPOINT")).trim();
                String identity = row.get("IDENTITY") == null ? null : String.valueOf(row.get("IDENTITY")).trim();
                if (endpoint == null || endpoint.isBlank() || identity == null || identity.isBlank()) {
                    continue;
                }

                byte[] psk = secretKeyService.findPSK(identity);
                if (psk == null || psk.length == 0) {
                    LOG.warn("Skipping persisted security mapping without PSK: endpoint={} identity={}", endpoint, identity);
                    continue;
                }

                try {
                    store.add(SecurityInfo.newPreSharedKeyInfo(endpoint, identity, psk));
                } catch (NonUniqueSecurityInfoException ignored) {
                    // Keep first loaded mapping when duplicate keys exist.
                }
            }
        } catch (NonUniqueSecurityInfoException e) {
            throw new IllegalStateException("Failed to preload PSK credentials", e);
        }
        return store;
    }

    @Bean
    public ObjectMapper objectMapper() {
        ObjectMapper mapper = new ObjectMapper();
        mapper.setSerializationInclusion(JsonInclude.Include.NON_NULL);
        SimpleModule module = new SimpleModule();
        module.addSerializer(Link.class, new JacksonLinkSerializer());
        module.addSerializer(Registration.class, new JacksonRegistrationSerializer(null));
        module.addSerializer(LwM2mResponse.class, new JacksonResponseSerializer());
        module.addSerializer(LwM2mNode.class, new JacksonLwM2mNodeSerializer());
        module.addDeserializer(LwM2mNode.class, new JacksonLwM2mNodeDeserializer());
        mapper.registerModule(module);

        return mapper;
    }

    @Bean
    public LwM2mModelProvider modelProvider() throws IOException, InvalidModelException, InvalidDDFFileException {
        List<ObjectModel> models = ObjectLoader.loadAllDefault();

        ResourcePatternResolver resolver = new PathMatchingResourcePatternResolver();
        Resource[] modelResources = resolver.getResources("classpath:/models/*.xml");

        List<String> resourceNames = Arrays.stream(modelResources)
                .map(Resource::getFilename)
                .filter(Objects::nonNull)
                .sorted()
                .collect(Collectors.toList());

        models.addAll(ObjectLoader.loadDdfResources("/models/", resourceNames.toArray(new String[0])));
        return new VersionedModelProvider(models);
    }

}
