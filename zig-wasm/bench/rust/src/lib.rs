//
// Browser benchmark workload for Rust zenoh (the community wasm port), mirroring bench/bench.c:
// same modes, same key expressions and the same "BENCH {json}" / READY / IDLE_START / IDLE_END /
// DONE lines on the console, so bench/run-one.mjs drives it exactly like the Zig build.
//
// Differences that follow from the API rather than from the port:
// * pub sends with `express(true)` (one message per WebSocket frame, like zenoh-pico without
//   zp_batch_start); pubbatch uses Rust zenoh's automatic batching. Both use congestion control
//   BLOCK so nothing is dropped.
// * The `threads` build runs the workload on the port's Application worker; the single-threaded
//   build runs it on the page's event loop.
//

use std::future::Future;
use std::sync::atomic::{AtomicU64, Ordering};

use futures::future::{self, Either};
use wasm_bindgen::prelude::*;
use zenoh::{qos::CongestionControl, Config, Session};

#[wasm_bindgen]
extern "C" {
    #[wasm_bindgen(js_namespace = performance, js_name = now)]
    fn performance_now() -> f64;
    #[wasm_bindgen(js_namespace = console, js_name = log)]
    fn console_log(s: &str);
    #[wasm_bindgen(js_namespace = console, js_name = error)]
    fn console_error(s: &str);
}

thread_local! {
    // performance.now() counts from each worker's own start; timeOrigin makes them comparable.
    static TIME_ORIGIN_MS: f64 = js_sys::Reflect::get(&js_sys::global(), &"performance".into())
        .and_then(|p| js_sys::Reflect::get(&p, &"timeOrigin".into()))
        .ok()
        .and_then(|v| v.as_f64())
        .unwrap_or(0.0);
}

fn now_us() -> f64 {
    (TIME_ORIGIN_MS.with(|o| *o) + performance_now()) * 1e3
}

fn out(line: &str) {
    console_log(line);
}

async fn sleep_ms(ms: u32) {
    zenoh_runtime::wasm_yield::sleep_ms(ms).await
}

/// `Some(output)` if `fut` completes within `ms`.
async fn timeout<F: Future>(ms: u32, fut: F) -> Option<F::Output> {
    let fut = std::pin::pin!(fut);
    let timer = std::pin::pin!(sleep_ms(ms));
    match future::select(fut, timer).await {
        Either::Left((v, _)) => Some(v),
        Either::Right(_) => None,
    }
}

fn err(e: impl std::fmt::Display) -> String {
    e.to_string()
}

fn fail(mode: &str, error: &str) -> String {
    out(&format!(r#"BENCH {{"mode":"{mode}","error":"{error}"}}"#));
    error.to_string()
}

// Latency: n round trips through the router to a native z_pong (test/ping -> test/pong).
async fn bench_ping(s: &Session, n: usize, size: usize, open_ms: f64) -> Result<(), String> {
    let publisher = s.declare_publisher("test/ping").await.map_err(err)?;
    let sub = s.declare_subscriber("test/pong").await.map_err(err)?;
    let data = vec![1u8; size];

    // Until the pong peer is ready, pings may be dropped.
    let start = now_us();
    let mut answered = false;
    while !answered && now_us() - start < 10e6 {
        publisher.put(data.clone()).await.map_err(err)?;
        answered = matches!(timeout(100, sub.recv_async()).await, Some(Ok(_)));
    }
    if !answered {
        return Err(fail("ping", "no pong"));
    }
    // Absorb pongs for any other pings still in flight from the loop above.
    let drain = now_us();
    while now_us() - drain < 300e3 {
        let _ = timeout(10, sub.recv_async()).await;
    }

    let warmup = 50;
    let mut rtt = Vec::with_capacity(n);
    let mut lost = 0;
    for i in 0..warmup + n {
        let t = now_us();
        publisher.put(data.clone()).await.map_err(err)?;
        let got = matches!(timeout(1000, sub.recv_async()).await, Some(Ok(_)));
        let dt = now_us() - t;
        if i < warmup {
            continue;
        }
        if got {
            rtt.push(dt);
        } else {
            lost += 1;
        }
    }
    if rtt.is_empty() {
        return Err(fail("ping", "all pings lost"));
    }
    rtt.sort_by(|a, b| a.total_cmp(b));
    let got = rtt.len();
    let mean = rtt.iter().sum::<f64>() / got as f64;
    out(&format!(
        r#"BENCH {{"mode":"ping","n":{got},"size":{size},"lost":{lost},"open_ms":{open_ms:.2},"min_us":{:.0},"p50_us":{:.0},"p90_us":{:.0},"p99_us":{:.0},"max_us":{:.0},"mean_us":{mean:.0}}}"#,
        rtt[0],
        rtt[got / 2],
        rtt[got * 9 / 10],
        rtt[got * 99 / 100],
        rtt[got - 1],
    ));
    Ok(())
}

// Send: put n `size`-byte messages as fast as possible; the native peer counts what arrives.
async fn bench_pub(s: &Session, n: usize, size: usize, batch: bool) -> Result<(), String> {
    let mode = if batch { "pubbatch" } else { "pub" };
    let publisher = s
        .declare_publisher("thr")
        .congestion_control(CongestionControl::Block)
        .express(!batch)
        .await
        .map_err(err)?;
    let start = now_us();
    while !publisher.matching_status().await.map_err(err)?.matching() {
        if now_us() - start > 10e6 {
            return Err(fail(mode, "no matching subscriber"));
        }
        sleep_ms(1).await;
    }
    let data = vec![1u8; size];

    let mut failed = 0u64;
    let start = now_us();
    for _ in 0..n {
        if publisher.put(data.clone()).await.is_err() {
            failed += 1;
        }
    }
    let send_ms = (now_us() - start) / 1e3;
    out(&format!(
        r#"BENCH {{"mode":"{mode}","n":{n},"size":{size},"failed":{failed},"send_ms":{send_ms:.2},"send_rate":{:.0}}}"#,
        n as f64 / (send_ms / 1e3)
    ));
    Ok(())
}

static RX: AtomicU64 = AtomicU64::new(0);
static FIRST_RX_US: AtomicU64 = AtomicU64::new(0);
static LAST_RX_US: AtomicU64 = AtomicU64::new(0);

fn on_thr() {
    let t = now_us().to_bits();
    if RX.fetch_add(1, Ordering::SeqCst) == 0 {
        FIRST_RX_US.store(t, Ordering::SeqCst);
    }
    LAST_RX_US.store(t, Ordering::SeqCst);
}

fn last_rx_us() -> f64 {
    f64::from_bits(LAST_RX_US.load(Ordering::SeqCst))
}

// Receive: wait for n messages from a native publisher (which starts after READY).
async fn bench_sub(s: &Session, n: usize) -> Result<(), String> {
    let _sub = s
        .declare_subscriber("thr")
        .callback(|_| on_thr())
        .await
        .map_err(err)?;
    out("READY");

    let start = now_us();
    loop {
        let rx = RX.load(Ordering::SeqCst);
        let now = now_us();
        if rx >= n as u64 || (rx == 0 && now - start > 30e6) || (rx > 0 && now - last_rx_us() > 5e6) {
            break;
        }
        sleep_ms(5).await;
    }
    let rx = RX.load(Ordering::SeqCst);
    let span_ms = (last_rx_us() - f64::from_bits(FIRST_RX_US.load(Ordering::SeqCst))) / 1e3;
    let rate = if rx > 1 && span_ms > 0.0 { (rx - 1) as f64 / (span_ms / 1e3) } else { 0.0 };
    out(&format!(
        r#"BENCH {{"mode":"sub","n":{n},"received":{rx},"recv_ms":{span_ms:.2},"recv_rate":{rate:.0}}}"#
    ));
    if rx < n as u64 {
        return Err(format!("received {rx} of {n}"));
    }
    Ok(())
}

// Idle: an open session with a subscriber and no traffic for duration_ms.
// The driver measures the browser's CPU time between IDLE_START and IDLE_END.
async fn bench_idle(s: &Session, duration_ms: usize) -> Result<(), String> {
    let _sub = s
        .declare_subscriber("idle/**")
        .callback(|_| ())
        .await
        .map_err(err)?;
    out("IDLE_START");
    let start = now_us();
    sleep_ms(duration_ms as u32).await;
    out("IDLE_END");
    out(&format!(
        r#"BENCH {{"mode":"idle","seconds":{:.3},"spins":0}}"#,
        (now_us() - start) / 1e6
    ));
    Ok(())
}

async fn bench_run(connect: String, mode: String, n: usize, size: usize, init_ms: f64) -> Result<(), String> {
    let mut config = Config::default();
    config.insert_json5("mode", r#""client""#).map_err(err)?;
    config
        .insert_json5("connect/endpoints", &format!(r#"["{connect}"]"#))
        .map_err(err)?;
    config.insert_json5("scouting/multicast/enabled", "false").map_err(err)?;

    let t = now_us();
    let s = match zenoh::open(config).await {
        Ok(s) => s,
        Err(_) => return Err(fail(&mode, "zenoh::open failed")),
    };
    let open_ms = (now_us() - t) / 1e3;

    let res = match mode.as_str() {
        "open" => {
            out(&format!(r#"BENCH {{"mode":"open","open_ms":{open_ms:.2},"init_ms":{init_ms:.2}}}"#));
            Ok(())
        }
        "ping" => bench_ping(&s, n, size, open_ms).await,
        "pub" => bench_pub(&s, n, size, false).await,
        "pubbatch" => bench_pub(&s, n, size, true).await,
        "sub" => bench_sub(&s, n).await,
        "idle" => bench_idle(&s, n).await,
        _ => Err(fail(&mode, "unknown mode")),
    };
    let _ = s.close().await;
    out("DONE");
    res
}

/// Runs one benchmark. `shim_url` is this build's wasm-bindgen JS, which the worker pool loads.
#[wasm_bindgen]
pub async fn bench(connect: String, mode: String, n: u32, size: u32, shim_url: String) -> Result<(), JsValue> {
    std::panic::set_hook(Box::new(|info| console_error(&format!("bench failed: {info}"))));
    let (n, size) = (n as usize, size as usize);
    #[cfg(feature = "threads")]
    {
        // Worker pool start-up (not part of open_ms; reported as init_ms in mode "open").
        let t = now_us();
        match zenoh_runtime::__zenoh_init_threaded_runtime_async(&shim_url, 10_000).await {
            Ok(true) => {}
            other => return Err(JsValue::from_str(&format!("worker pool did not start: {other:?}"))),
        }
        let init_ms = (now_us() - t) / 1e3;
        zenoh_runtime::ZRuntime::Application
            .spawn(bench_run(connect, mode, n, size, init_ms))
            .await
            .map_err(|e| JsValue::from_str(&e.to_string()))?
            .map_err(|e| JsValue::from_str(&e))
    }
    #[cfg(not(feature = "threads"))]
    {
        let _ = shim_url;
        bench_run(connect, mode, n, size, 0.0).await.map_err(|e| JsValue::from_str(&e))
    }
}
