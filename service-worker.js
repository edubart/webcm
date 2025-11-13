// Service Worker for routing /@// URLs to the VM
let vmClientId = null;

// Track which tab has the running VM
self.addEventListener('message', (event) => {
    if (event.data.type === 'registerVM') {
        vmClientId = event.source.id;
        console.log('[SW] VM registered:', vmClientId);
    } else if (event.data.type === 'unregisterVM') {
        if (vmClientId === event.source.id) {
            vmClientId = null;
            console.log('[SW] VM unregistered');
        }
    }
});

// Intercept fetch requests to /@// routes
self.addEventListener('fetch', (event) => {
    const url = new URL(event.request.url);
    const scope = new URL(self.registration.scope);
    const basePath = scope.pathname;

    // Check if this is a VM route (/@//)
    if (url.pathname.startsWith(basePath + '@//')) {
        event.respondWith(handleVMRequest(url, event.request, basePath));
    }
});

async function handleVMRequest(url, request, basePath) {
    // Extract VM URL from path
    const vmPath = url.pathname.slice((basePath + '@//').length);
    const vmUrl = 'http://' + vmPath + url.search + url.hash;

    console.log('[SW] Routing to VM:', vmUrl);

    // Check if VM is running
    if (!vmClientId) {
        console.log('[SW] No VM running, redirecting to main page');
        return Response.redirect(basePath, 302);
    }

    try {
        const vmClient = await clients.get(vmClientId);
        if (!vmClient) {
            console.log('[SW] VM client not found, redirecting to main page');
            vmClientId = null;
            return Response.redirect(basePath, 302);
        }

        // Forward request to VM tab via MessageChannel
        return await forwardToVM(vmClient, vmUrl, request);
    } catch (error) {
        console.error('[SW] Error forwarding to VM:', error);
        return new Response('VM Error: ' + error.message, {
            status: 503,
            headers: { 'Content-Type': 'text/plain' }
        });
    }
}

async function forwardToVM(vmClient, vmUrl, request) {
    const channel = new MessageChannel();

    // Read request body if present (parallel with message send)
    const bodyPromise = (request.method !== 'GET' && request.method !== 'HEAD')
        ? request.arrayBuffer()
        : Promise.resolve(null);

    return Promise.race([
        new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                reject(new Error('VM request timeout'));
            }, 30000);

            channel.port1.onmessage = (event) => {
                clearTimeout(timeout);
                const data = event.data;

                if (data.error) {
                    resolve(new Response(data.error, {
                        status: 502,
                        headers: { 'Content-Type': 'text/plain' }
                    }));
                    return;
                }

                // Response body is already Uint8Array (transferred, not copied)
                resolve(new Response(data.body, {
                    status: data.status,
                    statusText: data.statusText || '',
                    headers: new Headers(data.headers)
                }));
            };

            // Send message with body
            bodyPromise.then(body => {
                const msg = {
                    type: 'vmFetch',
                    url: vmUrl,
                    method: request.method,
                    headers: Object.fromEntries(request.headers.entries())
                };
                if (body) {
                    msg.body = body;
                    vmClient.postMessage(msg, [channel.port2, body]);
                } else {
                    vmClient.postMessage(msg, [channel.port2]);
                }
            }).catch(reject);
        })
    ]);
}

self.addEventListener('activate', (event) => {
    console.log('[SW] Service Worker activated');
    event.waitUntil(clients.claim());
});

self.addEventListener('install', (event) => {
    console.log('[SW] Service Worker installed');
    self.skipWaiting();
});
