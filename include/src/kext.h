/**
	// Common header for coordinating &..system extensions.
*/
#ifndef _SYSTEM_KERNEL_H_included_
#define _SYSTEM_KERNEL_H_included_ 1

struct ExtensionInterfaces {
	struct KPortsAPI *extif_kports;
	struct EndpointAPI *extif_nw_endpoint;
};
#endif
