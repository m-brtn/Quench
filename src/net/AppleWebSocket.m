// The Apple WebSocket backend of quench_net (macOS and iOS): one NSURLSession per net::WebSocket with an ephemeral
// configuration (no cookies, no cache, no stored credentials), an NSURLSessionWebSocketTask per connection and a
// delegate on a serial background queue that passes the open, each whole message, pongs and the end to the C++ side
// (AppleWebSocket.cpp), which queues them for the main thread. A task receives one whole message per receive call, so
// the delegate asks for the next one after each; the task's maximumMessageSize is the client's limit; pings are sent
// only when the client asks (NSURLSession sends none by itself).

#if !__has_feature(objc_arc)
#error "compile with ARC (-fobjc-arc, set by quench_configure_target)"
#endif

#import <Foundation/Foundation.h>

#include <stdint.h>

void quench_ws_apple_opened(void* sink, uint64_t generation, const char* protocol);
void quench_ws_apple_message(void* sink, uint64_t generation, int binary, const void* data, size_t size);
void quench_ws_apple_pong(void* sink, uint64_t generation);
void quench_ws_apple_closed(void* sink, uint64_t generation, int code, const void* reason, size_t size);
void quench_ws_apple_failed(void* sink, uint64_t generation, int domain, long code, int status, const char* message);
void quench_ws_apple_sink_release(void* sink);

// One connection: touched by the main thread (send, close, abort) and the delegate queue, guarded by the delegate.
@interface QuenchWsTask : NSObject
@property(nonatomic) uint64_t generation;
@property(nonatomic, strong) NSURLSessionWebSocketTask* task;
@property(nonatomic) BOOL finished;       // its end was reported (or it was aborted): nothing more is
@property(nonatomic) BOOL closeRequested; // the client sent its close frame
@property(nonatomic) int closeCode;       // the client's code
@property(nonatomic, strong) NSData* closeReason;
@property(nonatomic) BOOL tooLarge;            // a receive failed on the size limit
@property(nonatomic, strong) NSError* failure; // the first error of a receive or a send
@end

@implementation QuenchWsTask
@end

@interface QuenchWsDelegate : NSObject <NSURLSessionWebSocketDelegate> {
@public
    void* _sink;
    QuenchWsTask* _current;
}
@end

static void reportFailure(void* sink, QuenchWsTask* state, NSError* error, int status) {
    if (status != 0 && status != 101) {
        quench_ws_apple_failed(sink, state.generation, 3, 0, status, "");
        return;
    }
    if (state.tooLarge) {
        quench_ws_apple_failed(sink, state.generation, 2, 0, 0, "a message over the size limit");
        return;
    }
    if (error == nil) error = state.failure;
    if (error == nil) {
        quench_ws_apple_failed(sink, state.generation, 1, 0, 0, "the connection ended without a close frame");
        return;
    }
    if ([error.domain isEqualToString:NSPOSIXErrorDomain] &&
        (error.code == 32 || error.code == 54 || error.code == 57 || error.code == 60)) {
        // EPIPE, ECONNRESET, ENOTCONN, ETIMEDOUT: an open connection lost (seen as ENOTCONN when the server's TCP
        // connection closed without a close frame) is reported as NSURLErrorNetworkConnectionLost.
        quench_ws_apple_failed(sink, state.generation, 0, -1005, 0, error.localizedDescription.UTF8String);
        return;
    }
    const int domain = [error.domain isEqualToString:NSURLErrorDomain] ? 0 : 1;
    quench_ws_apple_failed(sink, state.generation, domain, (long)error.code, 0, error.localizedDescription.UTF8String);
}

@implementation QuenchWsDelegate

- (QuenchWsTask*)stateOf:(NSURLSessionTask*)task {
    @synchronized(self) {
        return (_current != nil && _current.task == task) ? _current : nil;
    }
}

// Marks the connection finished; NO when it already was.
- (BOOL)finish:(QuenchWsTask*)state {
    @synchronized(self) {
        if (state == nil || state.finished) return NO;
        state.finished = YES;
        return YES;
    }
}

- (void)receiveOn:(QuenchWsTask*)state {
    __weak QuenchWsDelegate* weakSelf = self;
    [state.task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* message, NSError* error) {
      QuenchWsDelegate* me = weakSelf;
      if (me == nil) return;
      void* sink = NULL;
      @synchronized(me) {
          if (state.finished) return;
          sink = me->_sink;
      }
      if (sink == NULL) return;
      if (error != nil) {
          // The end itself is reported by didCloseWithCode or didCompleteWithError; remember why.
          @synchronized(me) {
              if (state.failure == nil) state.failure = error;
              // The size limit's error is not documented: a POSIX EMSGSIZE (40) is what it was seen as.
              if ([error.domain isEqualToString:NSPOSIXErrorDomain] && error.code == 40) state.tooLarge = YES;
          }
          return;
      }
      if (message.type == NSURLSessionWebSocketMessageTypeString) {
          NSData* utf8 = [message.string dataUsingEncoding:NSUTF8StringEncoding];
          quench_ws_apple_message(sink, state.generation, 0, utf8.bytes, utf8.length);
      } else {
          quench_ws_apple_message(sink, state.generation, 1, message.data.bytes, message.data.length);
      }
      [me receiveOn:state];
    }];
}

- (void)URLSession:(NSURLSession*)session
          webSocketTask:(NSURLSessionWebSocketTask*)webSocketTask
    didOpenWithProtocol:(NSString*)protocol {
    (void)session;
    QuenchWsTask* state = [self stateOf:webSocketTask];
    if (state == nil || state.finished) return;
    quench_ws_apple_opened(_sink, state.generation, protocol ? protocol.UTF8String : "");
}

- (void)URLSession:(NSURLSession*)session
       webSocketTask:(NSURLSessionWebSocketTask*)webSocketTask
    didCloseWithCode:(NSURLSessionWebSocketCloseCode)closeCode
              reason:(NSData*)reason {
    (void)session;
    QuenchWsTask* state = [self stateOf:webSocketTask];
    if (![self finish:state]) return;
    quench_ws_apple_closed(_sink, state.generation, (int)closeCode, reason.bytes, reason.length);
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    (void)session;
    QuenchWsTask* state = [self stateOf:task];
    if (![self finish:state]) return;
    int status = 0;
    if ([task.response isKindOfClass:[NSHTTPURLResponse class]])
        status = (int)((NSHTTPURLResponse*)task.response).statusCode;
    NSURLSessionWebSocketTask* ws = (NSURLSessionWebSocketTask*)task;
    if (state.closeRequested && !state.tooLarge) {
        // Our close: the server's answer may have ended the task before a didCloseWithCode.
        const int code = ws.closeCode != NSURLSessionWebSocketCloseCodeInvalid ? (int)ws.closeCode : state.closeCode;
        NSData* reason = ws.closeReason ? ws.closeReason : state.closeReason;
        quench_ws_apple_closed(_sink, state.generation, code, reason.bytes, reason.length);
        return;
    }
    if (ws.closeCode != NSURLSessionWebSocketCloseCodeInvalid && !state.tooLarge && status == 101) {
        quench_ws_apple_closed(_sink, state.generation, (int)ws.closeCode, ws.closeReason.bytes, ws.closeReason.length);
        return;
    }
    reportFailure(_sink, state, error, status);
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
    if (sink) quench_ws_apple_sink_release(sink);
}

@end

@interface QuenchWsSession : NSObject
@property(nonatomic, strong) NSURLSession* session;
@property(nonatomic, strong) QuenchWsDelegate* delegate;
@end

@implementation QuenchWsSession
@end

static QuenchWsTask* currentTask(QuenchWsSession* s, uint64_t generation) {
    @synchronized(s.delegate) {
        QuenchWsTask* state = s.delegate->_current;
        if (state == nil || state.generation != generation || state.finished) return nil;
        return state;
    }
}

void* quench_ws_apple_open(void* sink) {
    NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    config.HTTPCookieStorage = nil;
    config.HTTPShouldSetCookies = NO;
    config.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
    config.URLCache = nil;
    config.URLCredentialStorage = nil;
    config.waitsForConnectivity = NO;
    config.timeoutIntervalForResource = 7 * 86400; // a connection lasts as long as the client keeps it
    QuenchWsDelegate* delegate = [[QuenchWsDelegate alloc] init];
    delegate->_sink = sink;
    NSOperationQueue* queue = [[NSOperationQueue alloc] init];
    queue.maxConcurrentOperationCount = 1; // in order
    queue.qualityOfService = NSQualityOfServiceUserInitiated;
    queue.name = @"quench.net.ws";
    QuenchWsSession* s = [[QuenchWsSession alloc] init];
    s.delegate = delegate;
    s.session = [NSURLSession sessionWithConfiguration:config delegate:delegate delegateQueue:queue];
    return (__bridge_retained void*)s;
}

void quench_ws_apple_close_session(void* handle) {
    QuenchWsSession* s = (__bridge_transfer QuenchWsSession*)handle;
    @synchronized(s.delegate) {
        if (s.delegate->_current) s.delegate->_current.finished = YES;
    }
    [s.session invalidateAndCancel];
}

int quench_ws_apple_connect(void* handle, uint64_t generation, const char* url, const char* const* names,
                            const char* const* values, size_t headerCount, const char* protocols,
                            int64_t maxMessageBytes, double timeoutSeconds) {
    QuenchWsSession* s = (__bridge QuenchWsSession*)handle;
    NSURL* address = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
    if (address == nil) return 1;
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:address];
    request.HTTPShouldHandleCookies = NO;
    request.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    request.timeoutInterval = timeoutSeconds > 0.0 ? timeoutSeconds : 86400.0;
    for (size_t i = 0; i < headerCount; ++i)
        [request setValue:[NSString stringWithUTF8String:values[i]]
            forHTTPHeaderField:[NSString stringWithUTF8String:names[i]]];
    if (protocols && *protocols)
        [request setValue:[NSString stringWithUTF8String:protocols] forHTTPHeaderField:@"Sec-WebSocket-Protocol"];
    NSURLSessionWebSocketTask* task = [s.session webSocketTaskWithRequest:request];
    task.maximumMessageSize = (NSInteger)maxMessageBytes;
    QuenchWsTask* state = [[QuenchWsTask alloc] init];
    state.generation = generation;
    state.task = task;
    @synchronized(s.delegate) {
        if (s.delegate->_current) {
            s.delegate->_current.finished = YES;
            [s.delegate->_current.task cancel];
        }
        s.delegate->_current = state;
    }
    [task resume];
    [s.delegate receiveOn:state];
    return 0;
}

void quench_ws_apple_send(void* handle, uint64_t generation, int binary, const void* data, size_t size) {
    QuenchWsSession* s = (__bridge QuenchWsSession*)handle;
    QuenchWsTask* state = currentTask(s, generation);
    if (state == nil) return;
    NSURLSessionWebSocketMessage* message = nil;
    if (binary) {
        message = [[NSURLSessionWebSocketMessage alloc] initWithData:[NSData dataWithBytes:data length:size]];
    } else {
        NSString* text = [[NSString alloc] initWithBytes:data length:size encoding:NSUTF8StringEncoding];
        if (text == nil) text = @""; // not UTF-8: the client sends text as given; nothing else can go out
        message = [[NSURLSessionWebSocketMessage alloc] initWithString:text];
    }
    QuenchWsDelegate* delegate = s.delegate;
    [state.task sendMessage:message
          completionHandler:^(NSError* error) {
            if (error == nil) return;
            @synchronized(delegate) {
                if (state.failure == nil) state.failure = error;
            }
          }];
}

void quench_ws_apple_ping(void* handle, uint64_t generation) {
    QuenchWsSession* s = (__bridge QuenchWsSession*)handle;
    QuenchWsTask* state = currentTask(s, generation);
    if (state == nil) return;
    QuenchWsDelegate* delegate = s.delegate;
    [state.task sendPingWithPongReceiveHandler:^(NSError* error) {
      void* sink = NULL;
      @synchronized(delegate) {
          if (state.finished || error != nil) return;
          sink = delegate->_sink;
      }
      if (sink) quench_ws_apple_pong(sink, state.generation);
    }];
}

void quench_ws_apple_close(void* handle, uint64_t generation, int code, const void* reason, size_t size) {
    QuenchWsSession* s = (__bridge QuenchWsSession*)handle;
    QuenchWsTask* state = currentTask(s, generation);
    if (state == nil) return;
    NSData* data = size > 0 ? [NSData dataWithBytes:reason length:size] : nil;
    @synchronized(s.delegate) {
        state.closeRequested = YES;
        state.closeCode = code;
        state.closeReason = data;
    }
    [state.task cancelWithCloseCode:(NSURLSessionWebSocketCloseCode)code reason:data];
}

void quench_ws_apple_abort(void* handle, uint64_t generation) {
    QuenchWsSession* s = (__bridge QuenchWsSession*)handle;
    QuenchWsTask* state = nil;
    @synchronized(s.delegate) {
        state = s.delegate->_current;
        if (state == nil || state.generation != generation) return;
        state.finished = YES;
        s.delegate->_current = nil;
    }
    [state.task cancel];
}
