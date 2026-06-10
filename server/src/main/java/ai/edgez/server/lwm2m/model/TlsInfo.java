package ai.edgez.server.lwm2m.model;

import ai.edgez.server.lwm2m.constant.TlsMode;

public class TlsInfo {
	private TlsMode mode;
	private TlsDetails details;
	public TlsMode getMode() {
		return mode;
	}
	public void setMode(TlsMode mode) {
		this.mode = mode;
	}
	public TlsDetails getDetails() {
		return details;
	}
	public void setDetails(TlsDetails details) {
		this.details = details;
	}
	
	
}
