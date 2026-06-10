package ai.edgez.server.lwm2m.model;

import org.eclipse.leshan.server.registration.Registration;
import java.util.Date;
import java.util.List;

public class RegistrationDto {
    private String endpoint;
    private String registrationId;
    private String address;
    private Date lastUpdate;
    private Date registrationDate;
    private List<String> objectLinks;

    public RegistrationDto(Registration reg) {
        this.endpoint = reg.getEndpoint();
        this.registrationId = reg.getId();
        this.address = reg.getAddress().toString();
        this.lastUpdate = reg.getLastUpdate();
        this.registrationDate = reg.getRegistrationDate();
        if (reg.getObjectLinks() != null) {
            this.objectLinks = java.util.Arrays.stream(reg.getObjectLinks())
                .map(link -> link.toString())
                .toList();
        }
    }

    public String getEndpoint() { return endpoint; }
    public String getRegistrationId() { return registrationId; }
    public String getAddress() { return address; }
    public Date getLastUpdate() { return lastUpdate; }
    public Date getRegistrationDate() { return registrationDate; }
    public List<String> getObjectLinks() { return objectLinks; }
}
