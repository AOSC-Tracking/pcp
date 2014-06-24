/*
 * Copyright (c) 2014 Red Hat.
 * 
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */
#include "pmapi.h"
#include "impl.h"
#include "internal.h"
#include "probe.h"

#if defined(IS_SOLARIS) && !defined(PTHREAD_STACK_MIN)
#define PTHREAD_STACK_MIN       ((size_t)_sysconf(_SC_THREAD_STACK_MIN))
#endif

/*
 * Service discovery by active probing. The given subnet is probed for the
 * requested service(s).
 */
typedef struct connectionOptions {
    __pmSockAddr	*netAddress;	/* Address of the subnet */
    int			maskBits;	/* Number of bits in the subnet */
    unsigned		maxThreads;	/* Max number of worker threads to use.*/
} connectionOptions;

/* Context for each thread. */
typedef struct connectionContext {
    const char		*service;	/* Service spec */
    __pmSockAddr	*nextAddress;	/* Next available address */
    int			maskBits;	/* Mask bits for the network address */
    int			nports;		/* Number of ports per address */
    int			portIx;		/* Index of next available port */
    const int		*ports;		/* The actual ports */
    int			*numUrls;	/* Size of the results */
    char		***urls;	/* The results */
    const __pmDiscoveryGlobalContext *globalContext;
#if PM_MULTI_THREAD
    __pmMutex		addrLock;	/* lock for the above address/port */
    __pmMutex		urlLock;	/* lock for the above results */
#endif
} connectionContext;

#if ! PM_MULTI_THREAD
/* Make these disappear. */
#undef PM_LOCK
#undef PM_UNLOCK
#define PM_LOCK(lock) do { } while (0)
#define PM_UNLOCK(lock) do { } while (0)
#endif

/*
 * Attempt connection based on the given context until there are no more
 * addresses+ports to try.
 */
static void *
attemptConnections(void *arg)
{
    int			s;
    int			flags;
    int			sts;
    __pmFdSet		wfds;
    __pmServiceInfo	serviceInfo;
    __pmSockAddr*	addr;
    int			port;
    int			attempt;
    struct timeval	canwait = {0, 100000 * 1000}; /* 0.1 seconds */
    connectionContext	*context = arg;

    /*
     * Keep trying to secure an address+port until there are no more
     * or until we are interrupted.
     */
    while (! context->globalContext->interrupted || ! *context->globalContext->interrupted) {
	/* Obtain the address lock while securing the next address, if any. */
	PM_LOCK(context->addrLock);
	if (context->nextAddress == NULL) {
	    /* No more addresses. */
	    PM_UNLOCK(context->addrLock);
	    break;
	}

	/*
	 * There is an address+port remaining. Secure them. If we cannot
	 * obtain our own copy of the address, then give up the lock and
	 * try again. Another thread will try this address+port.
	 */
	addr = __pmSockAddrDup(context->nextAddress);
	if (addr == NULL) {
	    PM_UNLOCK(context->addrLock);
	    continue;
	}
	port = context->ports[context->portIx];
	__pmSockAddrSetPort(addr, port);

	/*
	 * Advance the port index for the next thread. If we took the
	 * final port, then advance the address and reset the port index.
	 * The address may become NULL which is the signal for all
	 * threads to exit.
	 */
	++context->portIx;
	if (context->portIx == context->nports) {
	    context->portIx = 0;
	    context->nextAddress =
		__pmSockAddrNextSubnetAddr(context->nextAddress,
					   context->maskBits);
	}
	PM_UNLOCK(context->addrLock);

	/*
	 * Create a socket. There is a limit on open fds, not just from
	 * the OS, but also in the IPC table. If we get EAGAIN,
	 * then wait 0.1 seconds and try again.  We must have a limit in case
	 * something goes wrong. Make it 5 seconds (50 * 100,000 usecs).
	 */
	for (attempt = 0; attempt < 50; ++attempt) {
	    if (__pmSockAddrIsInet(addr))
		s = __pmCreateSocket();
	    else /* address family already checked */
		s = __pmCreateIPv6Socket();
	    if (s != -EAGAIN)
		break;
	    __pmtimevalSleep(canwait);
	}
	if (pmDebug & DBG_TRACE_DISCOVERY) {
	    if (attempt > 0) {
		__pmNotifyErr(LOG_INFO,
			      "Waited for %d attempts for an available fd\n",
			      attempt);
	    }
	}
	if (s < 0) {
	    char *addrString = __pmSockAddrToString(addr);
	    __pmNotifyErr(LOG_WARNING,
			  "__pmProbeDiscoverServices: Unable to create socket for address %s",
			  addrString);
	    free(addrString);
	    __pmSockAddrFree(addr);
	    continue;
	}

	/*
	 * Attempt to connect. If flags comes back as less than zero, then the
	 * socket has already been closed by __pmConnectTo().
	 */
	sts = -1;
	flags = __pmConnectTo(s, addr, port);
	if (flags >= 0) {
	    /* FNDELAY and we're in progress - wait on select */
	    __pmFD_ZERO(&wfds);
	    __pmFD_SET(s, &wfds);
	    canwait = *__pmConnectTimeout();
	    sts = __pmSelectWrite(s+1, &wfds, &canwait);

	    /* Was the connection successful? */
	    if (sts == 0)
		sts = -1; /* Timed out */
	    else if (sts > 0)
		sts = __pmConnectCheckError(s);

	    __pmCloseSocket(s);
	}

	/* If connection was successful, add this service to the list.  */
	if (sts == 0) {
	    serviceInfo.spec = context->service;
	    serviceInfo.address = addr;
	    if (strcmp(context->service, PM_SERVER_SERVICE_SPEC) == 0)
		serviceInfo.protocol = SERVER_PROTOCOL;
	    else if (strcmp(context->service, PM_SERVER_PROXY_SPEC) == 0)
		serviceInfo.protocol = PROXY_PROTOCOL;
	    else if (strcmp(context->service, PM_SERVER_WEBD_SPEC) == 0)
		serviceInfo.protocol = PMWEBD_PROTOCOL;

	    PM_LOCK(context->urlLock);
	    *context->numUrls =
		__pmAddDiscoveredService(&serviceInfo, context->globalContext,
					 *context->numUrls, context->urls);
	    PM_UNLOCK(context->urlLock);
	}

	__pmSockAddrFree(addr);
    } /* Loop over connection attempts. */

    return NULL;
}

static int
probeForServices(
    const char *service,
    const __pmDiscoveryGlobalContext *globalContext,
    const connectionOptions *options,
    int numUrls,
    char ***urls
)
{
    int			*ports = NULL;
    int			nports;
    int			prevNumUrls = numUrls;
    connectionContext	context;
#if PM_MULTI_THREAD
    int			sts;
    pthread_t		*threads = NULL;
    unsigned		threadIx;
    unsigned		nThreads;
    pthread_attr_t	threadAttr;
#endif

    /* Determine the port numbers for this service. */
    ports = NULL;
    nports = 0;
    nports = __pmServiceAddPorts(service, &ports, nports);
    if (nports <= 0) {
	__pmNotifyErr(LOG_ERR,
		      "__pmProbeDiscoverServices: could not find ports for service '%s'",
		      service);
	return 0;
    }

    /*
     * Initialize the shared probing context. This will be shared among all of
     * the worker threads.
     */
    context.service = service;
    context.ports = ports;
    context.nports = nports;
    context.numUrls = &numUrls;
    context.urls = urls;
    context.portIx = 0;
    context.maskBits = options->maskBits;
    context.globalContext = globalContext;

    /*
     * Initialize the first address of the subnet. This pointer will become
     * NULL and the mempry freed by __pmSockAddrNextSubnetAddr() when the
     * final address+port has been probed.
     */
    context.nextAddress =
	__pmSockAddrFirstSubnetAddr(options->netAddress, options->maskBits);
    if (context.nextAddress == NULL) {
	char *addrString = __pmSockAddrToString(options->netAddress);
	__pmNotifyErr(LOG_ERR,
		      "__pmProbeDiscoverServices: unable to determine the first address of the subnet: %s/%d",
		      addrString, options->maskBits);
	free(addrString);
	goto done;
    }

#if PM_MULTI_THREAD
    /*
     * Set up the concurrrency controls. These locks will be tested
     * even if we fail to allocate/use the thread table below.
     */
    pthread_mutex_init(&context.addrLock, NULL);
    pthread_mutex_init(&context.urlLock, NULL);

    /*
     * Allocate the thread table. We have a maximum for the number of threads,
     * so that will be the size.
     */
    threads = malloc(options->maxThreads * sizeof(*threads));
    if (threads == NULL) {
	/*
	 * Unable to allocate the thread table, however, We can still do the
	 * probing on the main thread.
	 */
	__pmNotifyErr(LOG_ERR,
		      "__pmProbeDiscoverServices: unable to allocate %u threads",
		      options->maxThreads);
    }
    else {
	/*
	 * We want our worker threads to be joinable and they don't need much
	 * stack. PTHREAD_STACK_MIN is not enough when resolving addresses,
	 * but twice that much is.
	 */
	pthread_attr_init(&threadAttr);
	pthread_attr_setdetachstate(&threadAttr, PTHREAD_CREATE_JOINABLE);
	pthread_attr_setstacksize(&threadAttr, 2 * PTHREAD_STACK_MIN);

	/* Dispatch the threads. */
	for (nThreads = 0; nThreads < options->maxThreads; ++nThreads) {
	    sts = pthread_create(&threads[nThreads], &threadAttr,
				 attemptConnections, &context);
	    /*
	     * If we failed to create a thread, then we've reached the OS limit.
	     */
	    if (sts != 0)
		break;
	}

	/* We no longer need this. */
	pthread_attr_destroy(&threadAttr);
    }
#endif

    /*
     * In addition to any threads we've dispatched, this thread can also
     * participate in the probing.
     */
    attemptConnections(&context);

#if PM_MULTI_THREAD
    if (threads) {
	/* Wait for all the connection attempts to finish. */
	for (threadIx = 0; threadIx < nThreads; ++threadIx)
	    pthread_join(threads[threadIx], NULL);

	/* These must not be destroyed until all of the threads have finished. */
	pthread_mutex_destroy(&context.addrLock);
	pthread_mutex_destroy(&context.urlLock);
    }
#endif

 done:
    free(ports);
    if (context.nextAddress)
	__pmSockAddrFree(context.nextAddress);
#if PM_MULTI_THREAD
    if (threads)
	free(threads);
#endif

    /* Return the number of new urls. */
    return numUrls - prevNumUrls;
}

/*
 * Parse the mechanism string for options. The first option will be of the form
 *
 *   probe=<net-address>/<maskSize>
 *
 * Subsequent options, if any, will be separated by commas. Currently supported:
 *
 *   maxThreads=<integer>  -- specifies a hard limit on the number of active
 *                            threads.
 */
static int
parseOptions(const char *mechanism, connectionOptions *options)
{
    const char		*addressString;
    const char		*maskString;
    size_t		len;
    char		*buf;
    char		*end;
    const char		*option;
    int			family;
    int			sts;
    long		longVal;
    unsigned		subnetBits;
    unsigned		subnetSize;

    /* Nothing to probe? */
    if (mechanism == NULL)
	return -1;

    /* First extract the subnet argument, parse it and check it. */
    addressString = strchr(mechanism, '=');
    if (addressString == NULL || addressString[1] == '\0') {
	__pmNotifyErr(LOG_ERR,
		      "__pmProbeDiscoverServices: No argument provided");
	return -1;
    }
    ++addressString;
    maskString = strchr(addressString, '/');
    if (maskString == NULL || maskString[1] == '\0') {
	__pmNotifyErr(LOG_ERR,
		      "__pmProbeDiscoverServices: No subnet mask provided");
	return -1;
    }
    ++maskString;

    /* Convert the address string to a socket address. */
    len = maskString - addressString; /* enough space for the nul */
    buf = malloc(len);
    memcpy(buf, addressString, len - 1);
    buf[len - 1] = '\0';
    options->netAddress = __pmStringToSockAddr(buf);
    if (options->netAddress == NULL) {
	__pmNotifyErr(LOG_ERR,
		      "__pmProbeDiscoverServices: Address '%s' is not valid",
		      buf);
	free(buf);
	return -1;
    }
    free(buf);

    /* Convert the mask string to an integer */
    options->maskBits = strtol(maskString, &end, 0);
    if (*end != '\0' && *end != ',') {
	__pmNotifyErr(LOG_ERR, "__pmProbeDiscoverServices: Subnet mask '%s' is not valid",
		      maskString);
	return -1;
    }

    /* Check the number of bits in the mask against the address family. */
    if (options->maskBits < 0) {
	__pmNotifyErr(LOG_ERR, "__pmProbeDiscoverServices: Inet subnet mask must be >= 0 bits");
	return -1;
    }
    family = __pmSockAddrGetFamily(options->netAddress);
    switch (family) {
    case AF_INET:
	if (options->maskBits > 32) {
	    __pmNotifyErr(LOG_ERR,
			  "__pmProbeDiscoverServices: Inet subnet mask must be <= 32 bits");
	    return -1;
	}
	break;
    case AF_INET6:
	if (options->maskBits > 128) {
	    __pmNotifyErr(LOG_ERR,
			  "__pmProbeDiscoverServices: Inet subnet mask must be <= 128 bits");
	    return -1;
	}
	break;
    default:
	__pmNotifyErr(LOG_ERR,
		      "__pmProbeDiscoverServices: Unsupported address family, %d",
		      family);
	return -1;
    }

    /*
     * Parse any remaining options.
     * Initialize to defaults first.
     *
     * FD_SETSIZE is the most open fds that __pmFD*()
     * and __pmSelect() can deal with, so it's a decent default for maxThreads.
     * The main thread also participates, so subtract 1.
     */
    options->maxThreads = FD_SETSIZE - 1;

    sts = 0;
    for (option = end; *option != '\0'; /**/) {
	/*
	 * All additional options begin with a separating comma.
	 * Make sure something has been specified.
	 */
	++option;
	if (*option == '\0') {
	    __pmNotifyErr(LOG_ERR,
			  "__pmProbeDiscoverServices: Missing option after ','");
	    return -1;
	    }
	
	/* Examine the option. */
	if (strncmp(option, "maxThreads=", 11) == 0) {
	    option += sizeof("maxThreads");
	    longVal = strtol(option, &end, 0);
	    if (*end != '\0' && *end != ',') {
		__pmNotifyErr(LOG_ERR,
			      "__pmProbeDiscoverServices: maxThreads value '%s' is not valid",
			      option);
		sts = -1;
	    }
	    else {
		option = end;
		/*
		 * Make sure the value is positive. Make sure that the given
		 * value does not exceed the existing value which is also the
		 * hard limit.
		 */
		if (longVal > options->maxThreads) {
		    __pmNotifyErr(LOG_ERR,
				  "__pmProbeDiscoverServices: maxThreads value %ld must not exceed %u",
				  longVal, options->maxThreads);
		    sts = -1;
		}
		else if (longVal <= 0) {
		    __pmNotifyErr(LOG_ERR,
				  "__pmProbeDiscoverServices: maxThreads value %ld must be positive",
				  longVal);
		    sts = -1;
		}
		else {
#if PM_MULTI_THREAD
		    options->maxThreads = longVal;
#else
		    __pmNotifyErr(LOG_WARNING,
				  "__pmProbeDiscoverServices: no thread support. Ignoring maxThreads value %ld",
				  longVal);
#endif
		}
	    }
	}
	else {
	    /* An invalid option. Skip it. */
	    __pmNotifyErr(LOG_ERR,
			  "__pmProbeDiscoverServices: option '%s' is not valid",
			  option);
	    sts = -1;
	    for (++option; *option != '\0' && *option != ','; ++option)
		;
	}
    } /* Parse additional options */

    /*
     * We now have a maximum for the number of threads
     * but there's no point in creating more threads than the number of
     * addresses in the subnet (less 1 for the main thread).
     *
     * We already know that the address is inet or ipv6 and that the
     * number of mask bits is appropriate.
     *
     * Beware of overflow!!! If the calculation would have overflowed,
     * then it means that the subnet is extremely large and therefore
     * much larger than maxThreads anyway.
     */
    if (__pmSockAddrIsInet(options->netAddress))
	subnetBits = 32 - options->maskBits;
    else
	subnetBits = 128 - options->maskBits;
    if (subnetBits < sizeof(subnetSize) * 8) {
	subnetSize = 1 << subnetBits;
	if (subnetSize - 1 < options->maxThreads)
	    options->maxThreads = subnetSize - 1;
    }

    return sts;
}

int
__pmProbeDiscoverServices(const char *service,
			  const char *mechanism,
			  const __pmDiscoveryGlobalContext *globalContext,
			  int numUrls,
			  char ***urls)
{
    connectionOptions options;
    int	sts;

    /* Interpret the mechanism string. */
    sts = parseOptions(mechanism, &options);
    if (sts != 0)
	return 0;

    /* Everything checks out. Now do the actual probing. */
    numUrls = probeForServices(service, globalContext, &options, numUrls, urls);

    /* Clean up */
    __pmSockAddrFree(options.netAddress);

    return numUrls;
}
