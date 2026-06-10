package ai.edgez.server.lwm2m.model;

import java.security.PublicKey;

import org.eclipse.leshan.core.oscore.OscoreSetting;
import org.eclipse.leshan.servers.security.SecurityInfo;

public class SecurityInfoDto{
	private String endpoint;
	private TlsInfo tls;
	
	

	public String getEndpoint() {
		return endpoint;
	}



	public void setEndpoint(String endpoint) {
		this.endpoint = endpoint;
	}

	public TlsInfo getTls() {
		return tls;
	}



	public void setTls(TlsInfo tls) {
		this.tls = tls;
	}



	public SecurityInfo toSecurityInfo() {
		return SecurityInfo.newPreSharedKeyInfo(endpoint,tls.getDetails().getIdentity(),tls.getDetails().getKey().getBytes());
	}
}
