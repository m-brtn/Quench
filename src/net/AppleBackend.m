// The Apple backend of quench_net (macOS and iOS): one NSURLSession per client with an ephemeral configuration (no
// cookies, no cache, no stored credentials), a data task per request and a delegate on a serial background queue that
// passes the headers, each piece of the body as it arrives and the end to the C++ side (AppleBackend.cpp), which
// queues them for the main thread. The delegate enforces the size limit while receiving and the redirect policy
// (quench_net_redirect_policy: the HTTPS rule, secret headers dropped on another origin).

#if !__has_feature(objc_arc)
#error "compile with ARC (-fobjc-arc, set by quench_configure_target)"
#endif

#import <Foundation/Foundation.h>

#include <stdint.h>

void quench_net_apple_headers(void* sink, uint64_t requestId, int status, const char* headerBlock,
                              const char* finalUrl);
void quench_net_apple_data(void* sink, uint64_t requestId, const void* data, size_t size);
void quench_net_apple_done(void* sink, uint64_t requestId);
void quench_net_apple_failed(void* sink, uint64_t requestId, int kind, long code, const char* message);
void quench_net_apple_sink_release(void* sink);
int quench_net_redirect_policy(const char* from, const char* to, int allowInsecure, char* reason, size_t size);
int quench_net_is_secret_header(const char* name);

enum { kMaxRedirects = 5 };

// One request: guarded by the delegate (@synchronized), touched by the main thread (cancel) and the delegate queue.
@interface QuenchNetTask : NSObject
@property(nonatomic) uint64_t requestId;
@property(nonatomic) uint64_t maxBytes;
@property(nonatomic) uint64_t received;
@property(nonatomic) int redirects;
@property(nonatomic) BOOL allowInsecure;
@property(atomic) BOOL finished;
@property(nonatomic, strong) NSURLSessionDataTask* task;
@end

@implementation QuenchNetTask
@end

@interface QuenchNetDelegate : NSObject <NSURLSessionDataDelegate> {
@public
    void* _sink;
    NSMutableDictionary<NSNumber*, QuenchNetTask*>* _byTask;    // taskIdentifier -> request
    NSMutableDictionary<NSNumber*, QuenchNetTask*>* _byRequest; // request id -> request
}
@end

@implementation QuenchNetDelegate

- (QuenchNetTask*)stateOf:(NSURLSessionTask*)task {
    @synchronized(self) {
        return _byTask[@(task.taskIdentifier)];
    }
}

// Marks the request finished; NO when it already was (cancelled or failed before).
- (BOOL)finish:(QuenchNetTask*)state {
    @synchronized(self) {
        if (state == nil || state.finished) return NO;
        state.finished = YES;
        return YES;
    }
}

- (void)forget:(QuenchNetTask*)state {
    @synchronized(self) {
        [_byTask removeObjectForKey:@(state.task.taskIdentifier)];
        [_byRequest removeObjectForKey:@(state.requestId)];
    }
}

- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)dataTask
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    (void)session;
    QuenchNetTask* state = [self stateOf:dataTask];
    if (state == nil || state.finished) {
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    int status = 0;
    NSMutableString* block = [NSMutableString string];
    if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
        NSHTTPURLResponse* http = (NSHTTPURLResponse*)response;
        status = (int)http.statusCode;
        [http.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL* stop) {
          (void)stop;
          [block appendFormat:@"%@\n%@\n", key, value];
        }];
    }
    if (response.expectedContentLength >= 0 && (uint64_t)response.expectedContentLength > state.maxBytes) {
        if ([self finish:state]) {
            NSString* message = [NSString stringWithFormat:@"the body of %lld bytes is over the limit of %llu",
                                                           response.expectedContentLength, state.maxBytes];
            quench_net_apple_failed(_sink, state.requestId, 2, 0, message.UTF8String);
        }
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    quench_net_apple_headers(_sink, state.requestId, status, block.UTF8String, response.URL.absoluteString.UTF8String);
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveData:(NSData*)data {
    (void)session;
    QuenchNetTask* state = [self stateOf:dataTask];
    if (state == nil || state.finished) return;
    state.received += data.length;
    if (state.received > state.maxBytes) {
        if ([self finish:state]) {
            NSString* message = [NSString stringWithFormat:@"the body is over the limit of %llu bytes", state.maxBytes];
            quench_net_apple_failed(_sink, state.requestId, 2, 0, message.UTF8String);
        }
        [dataTask cancel];
        return;
    }
    void* sink = _sink;
    const uint64_t requestId = state.requestId;
    [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
      (void)stop;
      quench_net_apple_data(sink, requestId, bytes, range.length);
    }];
}

- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completionHandler {
    (void)session;
    (void)response;
    QuenchNetTask* state = [self stateOf:task];
    if (state == nil || state.finished) {
        completionHandler(nil);
        return;
    }
    char reason[512] = {0};
    const char* from = task.currentRequest.URL.absoluteString.UTF8String;
    const char* to = request.URL.absoluteString.UTF8String;
    int policy =
        quench_net_redirect_policy(from ? from : "", to ? to : "", state.allowInsecure ? 1 : 0, reason, sizeof reason);
    state.redirects += 1;
    if (state.redirects > kMaxRedirects) {
        policy = 0;
        snprintf(reason, sizeof reason, "more than %d redirects", kMaxRedirects);
    }
    if (policy == 0) {
        if ([self finish:state]) quench_net_apple_failed(_sink, state.requestId, 3, 0, reason);
        completionHandler(nil);
        [task cancel];
        return;
    }
    if (policy == 2) {
        // Another origin: the Authorization, Cookie and key headers stay behind.
        NSMutableURLRequest* next = [request mutableCopy];
        for (NSString* name in request.allHTTPHeaderFields.allKeys)
            if (quench_net_is_secret_header(name.UTF8String)) [next setValue:nil forHTTPHeaderField:name];
        completionHandler(next);
        return;
    }
    // The same origin as the step before. NSURLSession leaves Authorization out of every redirected request (seen
    // on macOS 27): it is put back from the original request when the target has the original's origin too, as a
    // browser keeps it.
    NSURLRequest* original = task.originalRequest;
    const char* first = original.URL.absoluteString.UTF8String;
    if (quench_net_redirect_policy(first ? first : "", to ? to : "", state.allowInsecure ? 1 : 0, NULL, 0) == 1) {
        NSMutableURLRequest* next = [request mutableCopy];
        [original.allHTTPHeaderFields enumerateKeysAndObjectsUsingBlock:^(NSString* name, NSString* value, BOOL* stop) {
          (void)stop;
          if (quench_net_is_secret_header(name.UTF8String) && [next valueForHTTPHeaderField:name] == nil)
              [next setValue:value forHTTPHeaderField:name];
        }];
        completionHandler(next);
        return;
    }
    completionHandler(request);
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    (void)session;
    QuenchNetTask* state = [self stateOf:task];
    if (state == nil) return;
    if ([self finish:state]) {
        if (error == nil) {
            quench_net_apple_done(_sink, state.requestId);
        } else {
            const int domain = [error.domain isEqualToString:NSURLErrorDomain] ? 0 : 1;
            quench_net_apple_failed(_sink, state.requestId, domain, (long)error.code,
                                    error.localizedDescription.UTF8String);
        }
    }
    [self forget:state];
}

- (void)URLSession:(NSURLSession*)session didBecomeInvalidWithError:(NSError*)error {
    (void)session;
    (void)error;
    // The last call the session makes: the C++ side's queue may go.
    void* sink = NULL;
    @synchronized(self) {
        sink = _sink;
        _sink = NULL;
    }
    if (sink) quench_net_apple_sink_release(sink);
}

@end

@interface QuenchNetSession : NSObject
@property(nonatomic, strong) NSURLSession* session;
@property(nonatomic, strong) QuenchNetDelegate* delegate;
@end

@implementation QuenchNetSession
@end

void* quench_net_apple_open(void* sink) {
    NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    config.HTTPCookieStorage = nil;
    config.HTTPShouldSetCookies = NO;
    config.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
    config.URLCache = nil;
    config.URLCredentialStorage = nil;
    config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    config.waitsForConnectivity = NO;          // offline fails at once instead of waiting
    config.timeoutIntervalForResource = 86400; // the client's timeout ends a request
    QuenchNetDelegate* delegate = [[QuenchNetDelegate alloc] init];
    delegate->_sink = sink;
    delegate->_byTask = [NSMutableDictionary dictionary];
    delegate->_byRequest = [NSMutableDictionary dictionary];
    NSOperationQueue* queue = [[NSOperationQueue alloc] init];
    queue.maxConcurrentOperationCount = 1; // in order per request
    queue.qualityOfService = NSQualityOfServiceUserInitiated;
    queue.name = @"quench.net";
    QuenchNetSession* s = [[QuenchNetSession alloc] init];
    s.delegate = delegate;
    s.session = [NSURLSession sessionWithConfiguration:config delegate:delegate delegateQueue:queue];
    return (__bridge_retained void*)s;
}

void quench_net_apple_close(void* handle) {
    QuenchNetSession* s = (__bridge_transfer QuenchNetSession*)handle;
    @synchronized(s.delegate) {
        for (QuenchNetTask* state in s.delegate->_byRequest.allValues) state.finished = YES;
    }
    // Cancels the tasks; the delegate releases the sink in didBecomeInvalidWithError.
    [s.session invalidateAndCancel];
}

int quench_net_apple_start(void* handle, uint64_t requestId, const char* method, const char* url,
                           const char* const* names, const char* const* values, size_t headerCount, const void* body,
                           size_t bodySize, double timeoutSeconds, uint64_t maxBytes, int allowInsecure) {
    QuenchNetSession* s = (__bridge QuenchNetSession*)handle;
    NSURL* address = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
    if (address == nil) return 1;
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:address];
    request.HTTPMethod = [NSString stringWithUTF8String:method];
    request.HTTPShouldHandleCookies = NO;
    request.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    // NSURLSession's own timeout is between pieces of data; the client ends the whole request at its timeout.
    request.timeoutInterval = timeoutSeconds > 0.0 ? timeoutSeconds : 86400.0;
    for (size_t i = 0; i < headerCount; ++i)
        [request setValue:[NSString stringWithUTF8String:values[i]]
            forHTTPHeaderField:[NSString stringWithUTF8String:names[i]]];
    if (bodySize > 0) request.HTTPBody = [NSData dataWithBytes:body length:bodySize];
    NSURLSessionDataTask* task = [s.session dataTaskWithRequest:request];
    QuenchNetTask* state = [[QuenchNetTask alloc] init];
    state.requestId = requestId;
    state.maxBytes = maxBytes;
    state.allowInsecure = allowInsecure != 0;
    state.task = task;
    @synchronized(s.delegate) {
        s.delegate->_byTask[@(task.taskIdentifier)] = state;
        s.delegate->_byRequest[@(requestId)] = state;
    }
    [task resume];
    return 0;
}

void quench_net_apple_cancel(void* handle, uint64_t requestId) {
    QuenchNetSession* s = (__bridge QuenchNetSession*)handle;
    QuenchNetTask* state = nil;
    @synchronized(s.delegate) {
        state = s.delegate->_byRequest[@(requestId)];
        if (state == nil) return;
        state.finished = YES;
    }
    [state.task cancel];
}
