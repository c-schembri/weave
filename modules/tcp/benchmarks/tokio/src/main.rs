use std::io::{self, Write};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpSocket, TcpStream};
use tokio::runtime::Builder;

#[cfg(windows)]
#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetCurrentProcess() -> *mut std::ffi::c_void;
    fn SetProcessAffinityMask(process: *mut std::ffi::c_void, mask: usize) -> i32;
}

#[derive(Clone, Copy)]
struct Workload {
    bytes: usize,
    work: usize,
    uneven: bool,
}

fn transform(buffer: &mut [u8], config: Workload) {
    let seed = u64::from_le_bytes(buffer[..8].try_into().unwrap());
    let iterations = if config.uneven && (seed - 1) % 8 != 0 {
        0
    } else {
        config.work
    };
    let mut result = seed;
    for _ in 0..iterations {
        result = result.wrapping_mul(1664525).wrapping_add(1013904223);
        result ^= result >> 17;
    }
    buffer[..8].copy_from_slice(&result.to_le_bytes());
}

async fn session(mut client: TcpStream, config: Workload) -> io::Result<()> {
    let mut buffer = vec![0; config.bytes];
    loop {
        client.read_exact(&mut buffer).await?;
        transform(&mut buffer, config);
        client.write_all(&buffer).await?;
    }
}

async fn serve(config: Workload) -> io::Result<()> {
    let socket = TcpSocket::new_v4()?;
    socket.bind("127.0.0.1:0".parse().unwrap())?;
    let listener = socket.listen(8192)?;
    println!(
        "{{\"port\":{},\"workers\":4}}",
        listener.local_addr()?.port()
    );
    io::stdout().flush()?;
    loop {
        let (client, _) = listener.accept().await?;
        client.set_nodelay(true)?;
        tokio::spawn(async move {
            let _ = session(client, config).await;
        });
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = std::env::args().collect();
    if args.len() != 5 {
        return Err("Usage: weave-tokio-bench bytes cpu uneven affinity-mask".into());
    }
    let bytes = args[1].parse()?;
    let work = args[2].parse()?;
    let uneven: u8 = args[3].parse()?;
    let mask: usize = args[4].parse()?;
    if !(16..=65536).contains(&bytes) || work > 2_000_000 || uneven > 1 {
        return Err("Invalid workload".into());
    }
    #[cfg(windows)]
    if mask != 0 && unsafe { SetProcessAffinityMask(GetCurrentProcess(), mask) } == 0 {
        return Err(io::Error::last_os_error().into());
    }
    let config = Workload {
        bytes,
        work,
        uneven: uneven != 0,
    };
    let runtime = Builder::new_multi_thread()
        .worker_threads(4)
        .enable_io()
        .build()?;
    // Schedule the accept loop on a worker too; block_on itself is not a worker task.
    runtime.block_on(runtime.spawn(serve(config)))??;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn transformation_wraps_and_only_changes_header() {
        let mut frame = vec![0xa5; 16];
        frame[..8].copy_from_slice(&1u64.to_le_bytes());
        transform(
            &mut frame,
            Workload {
                bytes: 16,
                work: 2,
                uneven: false,
            },
        );
        let mut expected = 1u64;
        for _ in 0..2 {
            expected = expected.wrapping_mul(1664525).wrapping_add(1013904223);
            expected ^= expected >> 17;
        }
        assert_eq!(&frame[..8], &expected.to_le_bytes());
        assert_eq!(&frame[8..], &[0xa5; 8]);
        frame[..8].copy_from_slice(&2u64.to_le_bytes());
        transform(
            &mut frame,
            Workload {
                bytes: 16,
                work: 20_000,
                uneven: true,
            },
        );
        assert_eq!(&frame[..8], &2u64.to_le_bytes());
    }
}
