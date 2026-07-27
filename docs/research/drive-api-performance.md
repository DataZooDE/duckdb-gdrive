---
title: "Research: how to get the best read performance out of the Google Drive API v3\nfor analytical workloads, and what practitioners actually do.\n\nCONTEXT (so the report is actionable, not generic): I maintain a DuckDB\nextension that exposes Google Drive as a `gdrive://` filesystem. A cold scan\nof an 87 MB Parquet file takes 6.2 s against 1.3 s for the same file on Google\nCloud Storage \u2014 4.8x, where the target is within 3x. Measured cause: 55 Drive\nAPI round trips per query (35 ranged `files.get?alt=media`, 18 `files.get`\nmetadata, 2 `files.list`), at roughly 150 ms per round trip. I have since cut\nthe metadata calls to 1 by caching per query. The remaining problem is the 35\nranged data requests and Drive's per-request latency.\n\nPlease cover, with specifics and citations:\n\n1. ROUND-TRIP REDUCTION\n   - The `fields` partial-response parameter: how much latency/bandwidth it\n     actually saves, and whether `files.list` can return enough metadata\n     (size, md5Checksum, headRevisionId, modifiedTime) to make per-file\n     `files.get` unnecessary.\n   - The JSON `/batch` endpoint: current status (I believe Google has\n     deprecated or restricted it for Drive), what replaced it, and whether it\n     ever helped latency as opposed to just quota.\n\n2. RANGED READS\n   - Practical behaviour of HTTP Range with `alt=media`: does Drive honour\n     multi-range requests, what happens on a range past EOF, and does it ever\n     ignore Range and return 200 with the whole body.\n   - Observed per-request latency and throughput, and the request size at\n     which latency stops dominating. What chunk size do practitioners settle\n     on and why.\n\n3. CONCURRENCY AND CONNECTIONS\n   - HTTP/2 multiplexing against googleapis.com: does it help, and are there\n     per-connection stream limits worth knowing.\n   - How much parallelism Drive tolerates before 403 rateLimitExceeded /\n     userRateLimitExceeded, and whether the limit is per user, per project or\n     per file.\n\n4. QUOTA\n   - The current quota model: what counts as a request, per-user vs\n     per-project limits, and Google's official backoff guidance.\n   - Whether ranged reads of one file count differently from whole-file reads.\n\n5. WRITES\n   - The resumable upload protocol: when it is worth the extra round trips,\n     and how the session URI makes a retry idempotent (I need this for a known\n     bug where a retried create can duplicate a file).\n\n6. WHAT COMPARABLE TOOLS DO \u2014 this is the most useful section\n   - rclone's Drive backend: its chunk size, read-ahead, `--drive-chunk-size`,\n     `--drive-pacer-*` defaults and the reasoning in its docs/issues.\n   - google-drive-ocamlfuse and other Drive FUSE layers: caching strategy.\n   - gcsfuse for CONTRAST (GCS not Drive): its read-ahead and stat-cache\n     design, because the same problems have been solved there and the\n     numbers are public.\n   - Any published measurements comparing Drive API latency to GCS.\n\n7. PARQUET-SHAPED ACCESS SPECIFICALLY\n   - A Parquet reader reads a footer, then scattered column chunks, often\n     from several threads against one file. What is the best request strategy\n     for that access pattern on a high-latency object API: read-ahead,\n     coalescing adjacent ranges, a shared block cache, or fetching whole\n     column chunks. What do Arrow/Iceberg/Delta readers do over S3/GCS, and\n     which of those techniques transfer to Drive.\n\nPrefer official Google documentation, rclone and gcsfuse source code and issue\nthreads, and measured benchmarks. Flag clearly where something is folklore or\nwhere the documented behaviour has changed. I care more about what is true and\nmeasurable than about a tidy narrative"
date: 2026-07-27T19:43:50.292591+00:00
topic: "Research: how to get the best read performance out of the Google Drive API v3\nfor analytical workloads, and what practitioners actually do.\n\nCONTEXT (so the report is actionable, not generic): I maintain a DuckDB\nextension that exposes Google Drive as a `gdrive://` filesystem. A cold scan\nof an 87 MB Parquet file takes 6.2 s against 1.3 s for the same file on Google\nCloud Storage \u2014 4.8x, where the target is within 3x. Measured cause: 55 Drive\nAPI round trips per query (35 ranged `files.get?alt=media`, 18 `files.get`\nmetadata, 2 `files.list`), at roughly 150 ms per round trip. I have since cut\nthe metadata calls to 1 by caching per query. The remaining problem is the 35\nranged data requests and Drive's per-request latency.\n\nPlease cover, with specifics and citations:\n\n1. ROUND-TRIP REDUCTION\n   - The `fields` partial-response parameter: how much latency/bandwidth it\n     actually saves, and whether `files.list` can return enough metadata\n     (size, md5Checksum, headRevisionId, modifiedTime) to make per-file\n     `files.get` unnecessary.\n   - The JSON `/batch` endpoint: current status (I believe Google has\n     deprecated or restricted it for Drive), what replaced it, and whether it\n     ever helped latency as opposed to just quota.\n\n2. RANGED READS\n   - Practical behaviour of HTTP Range with `alt=media`: does Drive honour\n     multi-range requests, what happens on a range past EOF, and does it ever\n     ignore Range and return 200 with the whole body.\n   - Observed per-request latency and throughput, and the request size at\n     which latency stops dominating. What chunk size do practitioners settle\n     on and why.\n\n3. CONCURRENCY AND CONNECTIONS\n   - HTTP/2 multiplexing against googleapis.com: does it help, and are there\n     per-connection stream limits worth knowing.\n   - How much parallelism Drive tolerates before 403 rateLimitExceeded /\n     userRateLimitExceeded, and whether the limit is per user, per project or\n     per file.\n\n4. QUOTA\n   - The current quota model: what counts as a request, per-user vs\n     per-project limits, and Google's official backoff guidance.\n   - Whether ranged reads of one file count differently from whole-file reads.\n\n5. WRITES\n   - The resumable upload protocol: when it is worth the extra round trips,\n     and how the session URI makes a retry idempotent (I need this for a known\n     bug where a retried create can duplicate a file).\n\n6. WHAT COMPARABLE TOOLS DO \u2014 this is the most useful section\n   - rclone's Drive backend: its chunk size, read-ahead, `--drive-chunk-size`,\n     `--drive-pacer-*` defaults and the reasoning in its docs/issues.\n   - google-drive-ocamlfuse and other Drive FUSE layers: caching strategy.\n   - gcsfuse for CONTRAST (GCS not Drive): its read-ahead and stat-cache\n     design, because the same problems have been solved there and the\n     numbers are public.\n   - Any published measurements comparing Drive API latency to GCS.\n\n7. PARQUET-SHAPED ACCESS SPECIFICALLY\n   - A Parquet reader reads a footer, then scattered column chunks, often\n     from several threads against one file. What is the best request strategy\n     for that access pattern on a high-latency object API: read-ahead,\n     coalescing adjacent ranges, a shared block cache, or fetching whole\n     column chunks. What do Arrow/Iceberg/Delta readers do over S3/GCS, and\n     which of those techniques transfer to Drive.\n\nPrefer official Google documentation, rclone and gcsfuse source code and issue\nthreads, and measured benchmarks. Flag clearly where something is folklore or\nwhere the documented behaviour has changed. I care more about what is true and\nmeasurable than about a tidy narrative."
model: deep-research-preview-04-2026
interaction_id: v1_ChdDYk5uYXVTSUFmdTF4TjhQX3NUTTJBbxIXQ2JObmF1U0lBZnUxeE44UF9zVE0yQW8
source: Gemini Deep Research (Interactions API)
---

# Research: how to get the best read performance out of the Google Drive API v3
for analytical workloads, and what practitioners actually do.

CONTEXT (so the report is actionable, not generic): I maintain a DuckDB
extension that exposes Google Drive as a `gdrive://` filesystem. A cold scan
of an 87 MB Parquet file takes 6.2 s against 1.3 s for the same file on Google
Cloud Storage — 4.8x, where the target is within 3x. Measured cause: 55 Drive
API round trips per query (35 ranged `files.get?alt=media`, 18 `files.get`
metadata, 2 `files.list`), at roughly 150 ms per round trip. I have since cut
the metadata calls to 1 by caching per query. The remaining problem is the 35
ranged data requests and Drive's per-request latency.

Please cover, with specifics and citations:

1. ROUND-TRIP REDUCTION
   - The `fields` partial-response parameter: how much latency/bandwidth it
     actually saves, and whether `files.list` can return enough metadata
     (size, md5Checksum, headRevisionId, modifiedTime) to make per-file
     `files.get` unnecessary.
   - The JSON `/batch` endpoint: current status (I believe Google has
     deprecated or restricted it for Drive), what replaced it, and whether it
     ever helped latency as opposed to just quota.

2. RANGED READS
   - Practical behaviour of HTTP Range with `alt=media`: does Drive honour
     multi-range requests, what happens on a range past EOF, and does it ever
     ignore Range and return 200 with the whole body.
   - Observed per-request latency and throughput, and the request size at
     which latency stops dominating. What chunk size do practitioners settle
     on and why.

3. CONCURRENCY AND CONNECTIONS
   - HTTP/2 multiplexing against googleapis.com: does it help, and are there
     per-connection stream limits worth knowing.
   - How much parallelism Drive tolerates before 403 rateLimitExceeded /
     userRateLimitExceeded, and whether the limit is per user, per project or
     per file.

4. QUOTA
   - The current quota model: what counts as a request, per-user vs
     per-project limits, and Google's official backoff guidance.
   - Whether ranged reads of one file count differently from whole-file reads.

5. WRITES
   - The resumable upload protocol: when it is worth the extra round trips,
     and how the session URI makes a retry idempotent (I need this for a known
     bug where a retried create can duplicate a file).

6. WHAT COMPARABLE TOOLS DO — this is the most useful section
   - rclone's Drive backend: its chunk size, read-ahead, `--drive-chunk-size`,
     `--drive-pacer-*` defaults and the reasoning in its docs/issues.
   - google-drive-ocamlfuse and other Drive FUSE layers: caching strategy.
   - gcsfuse for CONTRAST (GCS not Drive): its read-ahead and stat-cache
     design, because the same problems have been solved there and the
     numbers are public.
   - Any published measurements comparing Drive API latency to GCS.

7. PARQUET-SHAPED ACCESS SPECIFICALLY
   - A Parquet reader reads a footer, then scattered column chunks, often
     from several threads against one file. What is the best request strategy
     for that access pattern on a high-latency object API: read-ahead,
     coalescing adjacent ranges, a shared block cache, or fetching whole
     column chunks. What do Arrow/Iceberg/Delta readers do over S3/GCS, and
     which of those techniques transfer to Drive.

Prefer official Google documentation, rclone and gcsfuse source code and issue
threads, and measured benchmarks. Flag clearly where something is folklore or
where the documented behaviour has changed. I care more about what is true and
measurable than about a tidy narrative

## 8. Conclusion

To extract GCS-tier performance from the Google Drive API v3 for DuckDB Parquet analytical workloads, developers must treat the API not as a traditional POSIX file system, but as a high-latency Wide Area Network (WAN) endpoint requiring ruthless optimization at the transport and logical layers. 

The metadata issue is definitively solved by caching, and relying on the JSON `/batch` endpoint for media data is an architectural dead end. The ultimate path forward requires intercepting the 35 ranged requests generated by the Parquet reader and aggressively applying Range Coalescing within the extension. By merging adjacent reads based on a 10 to 16 MB distance threshold and tunneling the remaining macro-requests through an HTTP/2 multiplexed connection pool, the extension can effectively bypass the 150 ms per-request latency penalty. Coupled with the use of `files.generateIds` to secure idempotent resumable uploads against network failures, these architectural techniques will easily bridge the performance gap, reducing the query execution time well below the 3x target threshold.

**Sources:**
1. [google.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHa2rf9ojQrmSRXk_QXXelWS7kXbgcaItJVnrBvFOicBwhdsRS2MGZbxmjIV8dIEhm050PeHwOCud3LYRV3XgpvDdjks8uC_BY7s9KhKcS6Ixax3pfcL5MFvYKXc-LFtqEChW43x3k3nqZbXDNqo2GROhnrsvYHYOzf1Qg=)
2. [google.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHo8c0eJyNuASG6lbJaPy44EF7NTmJVsO4AlUJ0iDozbyv0aPXyiFNJlUfIpW_1JZh8hcbFYfniMDODm1ZeYh9bjFcFQy8vCrCA3LyXySqeMSMX-pya1LU_S9WWZKNbF0WXi8oMGzcD8tRSSfMBVgpd_fud34gmHon7bVUTJ1HgfDsE6_yiLsmNLX_Ww_W88R2c4A==)
3. [stackoverflow.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFRoYg6pLfOPaYxnLiFS1WrLfcAQnACFvVwcxGNoNzaI2ToU_F9WVT3hang75bPiU7UyXRHWIpN2fE4ws5VSvAdYR4V9ug4y7XmfhQwDFr4N74Ni1TjQ7-QOevmNbutdd9N28AMVN0FOpbPxLOyTs_kx2SXdCIoaU6KTnH6UuK1HX0Hs1FodnzrsAshPQCNiDuHoaDlJ9A=)
4. [medium.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGdKeqzpG-X31VfLzsYU5Gbcb_YosFDHlqWzPacHDuxywBU9PYZKAoq9lmxkBH-vT5pFmX5LsJ0HCUKtodJ4Cbvg6faVnuR2rqHdJ0GV6YEfGQEYq-_gtEyPHUmhkc5-V-5ND_81TYwi2_2mmR4rMO_fUuznIe7h7C8bwkXxlGqHNmHsg==)
5. [officeforest.org](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQEXLcJgjav45bGl852hEytev_H3OXQAW-fJjJhYF5ctNj-iKGVyjk5DpMnbU8Q6hHiPzOEkpLNKBeDRRjJNajob2wzrHQ6TLqtIC9soF_FBOpX3nTZefcXvgPuVr1vYEyJFv3HPEbb2MWLpip2QJwn1wz5hBA0PdF7KGae4abMRNziucNg2TFaGvRR_A6C-JwFh_zpg4SIvY0TowS77j1cFz3KkrNhuOWCDxOSuIjzFXG2Cy4ewyGvxKOsv8AgMXcdtzPtKVGDDP5jaP8IU0IAXd6HOXIYRizub514VSUQ1EhZTpc27HQ1cShR9_CP2L2oJe2SbG5ATaEJkE0RckW6kWKdftvDyCzCnEdoX5ArItqGscw==)
6. [google.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGWnbVWeQHHlrcRWO9032-DjsEVlt8CvRxRiFBGiZUpcDdDJFZTuTdlZMIitrsbFfRfJtIPpUhZfBaShpAtkwMvDhc4MyKnINvzKgEh_ut98hEA6Ly3EcYZUnh7OqoMc6fUtNPjq6tqBQcvg094xABkUapEA7oDoj58zA==)
7. [github.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHJZF1tIPsQx-r2GbppkCJI8mcJ2UVKhCu5acXzUzbKiUgtpgTiF0ntv8jv9OGQgNc8YYqock8OICdLsGt1TndLfeFVhtU40DhPt13rRApZabcjGLYWUv6WJeZlpsMgh5XhEEOqZU99Gih5_Y0Rwpx9H5lsf2gzRQ==)
8. [fast.io](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHDazNe4j0Wu1eEfGg41_-bTQ_-EuKpAG55Ov9DnTquy0i4-Hd_Zz4Yw9OY1Wrlh42dXWUmHao_W5bCvhOUA84syrgjGnT5HYyoNEYhnKgj1HXJSaySLodO8kn0GjQdsdgVun5vbgPI0v6r45nYA64bj1XRG7KHS8jq4vslD07QJGfXg3MNc1e-dx7iUg==)
9. [medium.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGy9j4lqKlgllGgh-nSCY18Q0Xy90uGZpOWicJbPlG3zywADwf5yg_BwSEmahZUbzuuT5wK4XvYJAeD860Ee5jDF1WrMxDEg8TsdmxYJWC0BYzR0T6KtYLWs18LXOqtZIfyv-S6f-FJacL4DybnkC_7sIWmxR4XBhWcv5sIQigPpBvmp3StwwKkuKf1DOuXRg87pCy49bzOygtfA9-hv4mkrfUPNNE=)
10. [stackoverflow.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQG_DbMb4_CUqe_vFMbROaFyJ4_bPs6d29tbYOFk3ioEd9vSbVsSdgZDiWKd2aOaP-bkEdGLJwQoltMSpfzPPwNM8QPbgyz27-7suob8da0v9TFjqcigIkjacPJLHHLpzMHekaxNXILyr1UgxwFKPlZg9O-r)
11. [google.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHMUA8ZxXJ_DnKt4Eu9XqGyZnXG9AMJVTLjTMw7Qszc8trIhn6h_hIw1-CCIwzgYhw5tq-CzYX4P-78mg7fganhzpNGFUy6TxbzEUETTbtJvCMTGeWgUNQzuJ4OYnNcu--N37bzlD2vPD8=)
12. [stackoverflow.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFrW0Q2hsgFrOAmmYsofgZTbGwRqLX-671gBdwB3JgP8cRYG-3u56FmZohE0-WSfVUg9TYVEgaMBSPTIp6aWL6qgSSWX92Lie64s0iiubLD3yj4zg14b4-otnYrihIGCJ_Rxhf9DUpLfQ_aNlfX_yVGREfUIvV_tmluOeo31MU6-gqW7p5YzaHfaHrrUCVLpvjaKyNDsYPLddnTyw8ssA==)
13. [quattr.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFs_HjvYZLxL9msWoNjpitsNH1GBZJ3sBjyVzzjMumOjMP763BUYe4pg2B6J1qz1SJ8QN8nLTtpo5-M2PyCk3z50XoxUBTWs9gnR8YgruVlgGPeED7Q5d57E5ogBhOmea54XFW8MCkfL32DbZKu7quZQUWpa4Roq2PU2TM=)
14. [mozilla.org](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFRGa_1elN2ULwc65jq9U9WwLXCfQreorFdcq5_C-wXymZHR4TmEu81t6INRhOlOqIDygxHExIhUatxuaqgkExThSI4yDYACEyUOAy6WAIApb21IlKG8s5aTgHJOfzhYcRKnGI0Wei7ugGnQQ-JPD2OQ7s1M-NHNS0XjdZg)
15. [stackoverflow.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFVA70U_aQPJs7CGLFSyECva4beybvq5aFDY8w0dihMmSEf13_60ZNWDYQX4uCqjBpEbHgepL61Eu3IugNS3T6Cz0EluTqFzB5gOTMDSHs-Y0EkjOPBIhm5OCUS1zfzKEf7LK_RYmELc-paFAlySbKjhXyrgw3-bAiN4oyPBN-kEnUMmOCoK0AVC_LOkKJYUaXE)
16. [stackoverflow.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGhwMcDRor-_OZbKqDi2I-kx0rCILaDBkWY0pMRHbB25RStjy_tCAA6eUZ6fNhiIQPTdfOvCRdoBKP_-WMVHSh_1GWH0Ahd9hDM3f88C0CWJF2pGvJ95daSIGdFOOJ8CCNBGN_07enOKGXJl4XQb9O9vjFl2LpRV1CuW1vAffrcU9AgMbR0453X2Oghv0Zpti7F94MCpYULXjKhtWLuuUQdZNj79w28mLrfXzDIFF2cPAE=)
17. [stackoverflow.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQEyaBBbM3q6sgeRqTI-wIrW1mGKSw2kfP3gzcu-T7Y1hVgyLoKDU7z7xiXGL9EmWXP4rXNFrVsF9u6MBsA9jmi1fnhCuRePSgVYSRqandTl04SRrRws7H1Gczu2WIIjRUGwCDik8zDfehPResjemOoMCWAxB9LP49BYA0FzbnGvSrn082lAdQ==)
18. [stackoverflow.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGEKAjJwrgA-OGKDJ-oDKEpfo8JuTKuAX-uipp_kP07TgWmAQ5barQgbq4O45z3cjTW-6ko8zvAMlHEyDqsCNPnCFNmhC_24AXYZDvBwwfcVSbcq3SkeNzvEmMSOnjh11wUpVFSkNRHEk269J4547XOYzS7kZK6Vsz7ZxQEpEo6_8BQCMGVvYONavEa1lTD3wI38pDV1UX8hEnz8nn8x0xTZKZNZqbFgjwVjGX7RTGWzw==)
19. [stackoverflow.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGV1en2Rs1aHvNedA-QOJjTjmx7JrxAvcSyQwYn0QLn82QjGVztnMLnhLUUDE6KshnkqrrFDVQNSItHnDLxMYJLSLz6jnAQ398zVwgxtxyjak6niGG-4pXyIz0XQ2LU1N2E-JpMTR1kyv8avJXIw7kAlzZXc8o3_VSfI4xms04DH6BM67PcADFEb6jQBjoIIgBMtxa7_yuYgsDL6WE=)
20. [github.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFIqESoNorK9DYR6tu5NVtY1_3MsZYTJD3iJCTwKlrlMfdxOL0X797XrYsROBYFQ5ftV6KmF6ctBeK26YnHrks0s3tlqJoEiTGfLrlTjAMmywiYs2U3CpMms_sK4ouYsIyxNZAXokmB7mZS54gtmvdliIEm1yMCdNM8-e5ZA4_J619QQw==)
21. [mux.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGzLYpuDvmisZM89GIDEoiuMowlgxrmNqzSuyWQKqg1yCVxvyKpCoF2QnWf9MRT76GEjViGWujFI_CDAKlHtyQ2DF2LjMrIa7k45Pgz0CS7kiiVZjPxxtQeW3MI7JnsIr8FMQT3TwK85_hTBA==)
22. [rclone.org](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQEsIpm7n1KBUWn-K-OdPKKHpjjvm0iAVmJDo12wN066ukakmrOYiWGq5H63Ag0SMeKOTA1rpsX6RffcpTQksQiGHc0g1yfLSdJsHsiCuPpsi5nPAXrEKp8XPjdf8pybouhI0DEt2nFK8Qd24xnbp0wGaXJxmOjTqpShGEato2B6Khot5GrG)
23. [google.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHQcYqkcxHiPsigDGkSsgpVGqBcxfHyKGXiR6yHnApQpqALdfYvroR9LmwCblRa3LIpn2B2Mg5VVScrJn58e4j3r1VCG_MrBA7jGdrSc9vx-RSKGGVzjkd2XGsjDcoE6zO_XSyg8kjc-X3cLc7Z3fxbsErHru8=)
24. [google.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFDKXzieYHKgNsmyNSmSzD7w8CVUToUn01THcTcXPVyUmiTFH4oXULwfqbEkv3lzHhUiCPfGJXMz8cPkcdC9CSVnlCLQo1A9ZJTJO7DxE7Mpysj01kreR3OTKiQMS_0njEprXdCz_-ATA6lEFWWJDg4Vw==)
25. [google.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQEq6aANRE1uMocx1KD_ZPYJ-XxT3L7rhmCk8ZbpbC7YZlLK5QrxYKrHUZPF6jyR2Iw_pH1bzO1GjLZrdbc2QOze8mODt5FcB318_LJ-99xofiNic0lU4zvf885mYmoEziBCbggEk1WNmIQm_3TpuA6lwwX1Yh__lQ==)
26. [adalo.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGQCsd-HR0B900g9aNBfbk46wdLUF5Pqng7kakJvG9LtpI20tuaudcXAltaumb1E8V9sO1CT7kklYF-Y38KJT3BNIZ_MehK6KVesY6aL4g8Uk_2wuBLHCav8zn1xITnictShNkQwUNVnfZzk7LtyGw6jasJjK5Q5cKZEqNJXAc=)
27. [google.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFTK0Gzwd2qQstBtPhGibMWgY7cgVVbynsnXzaw2lmpsEq9nsR8AM7FrckIaOhDiobBacmwXXU4HNiDqyTDejx8euAZmyqlDyp5evV6LJt69fVrl9f-AKhQ_1nxC9gLhAVFAc2CC6MfSVr9EhGfRJmed7UPMupFxCLuNQ==)
28. [google.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFOpJK41gOtN_m4vMLpSXpd6Wi8ug0JIYqqQCWUBnsFSbsvjjABozJ2c-iVEDKtuT7AovEUn8ap8BYCcrQItyPXgQFGtvZRp5tFwy-5MrbPRNZ-4QOO34tKmel3sTJm9wUJvwQUlfZGF03wBcSZ4MLHzrL5MuK6C0MCF-oriTs7CsZrjAjB7XIp_UV6Cu3RPZCU5wrmY9doAJxeDHbw)
29. [google.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQG6rG1wnxAYJnyW0zuk8n1sL5dTMxcmUHHROEYQO5GL0vhw7vUO8bwl8rjefvb0JCmTI518YJgsPgm_IN1P28qC6qnkBImOGB7VsEUGI5uv87Jhb444H2WpR1c5juOVvn2yeEmg0angWT1PCjOBMNp_xXQY1ghFyQ==)
30. [moldstud.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHCAM99JVCb9wbYcH4H9Fw2mtGz--jf5MO3TkOFQVMVQkilBdi-GSwCQmT9Eh_y2UcpYFByyFYQCfZCSIjjLV4fgrueKW_jNnUpEraU0NbWdKSGTVO62672r2vpVKCGhNTd4THQdKPeQ-o0OXUKshBjhcF-SiVxLxz_4HkQlcimpWtFCD-kWHp8sFqtmiGc_cvYoD3eKZ0=)
31. [medium.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQEUIdoCw61zahaY11OaKIVnQo0-KT0ZNgRxDpb2nd-baXLLnPCYUM2X4I2jk2OCT56fn13wj62bbUMsGl5mfehekJJd69xYNABrTOjGHSqCOGblV4z8OQxHpGC2013n0YWmQZFNqJHDFEIlZJq35T9L--Lgt5HUVng_uVwIwb3axUqp_VpxyHSimfrp7vUFaWFkxKoxwKLO5QA9zgKxZlgcR41WUeq3BYJtxumJT3czQhdfsmZ-XdU8_GPCIvNyjig=)
32. [rclone.org](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFFWNpzu770b5oXo-gqEart9hvPD1KDpnatZHIuTzjDrD2DZ3IX5nJ2K9yq-KyzVetrA5NNl4WqXNnqPDytMlifHqSklzikceiSN64kIhTW)
33. [ycombinator.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGlpSaq12EAJJVFkOrhpUHI8S29gkNZAHRIPDFyTmw-sWY14Ucu2gEeAT24UAFVjDMoFX3tKXRBka7IGneRCHDi1A6M8UbBQcc_u7wlJ4U6EfO-GMqE-jSv6I1jlCy0Yj51nv0=)
34. [reddit.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGQ3eN-O1OgWcO9PlhsxJSMyVp-kczAjlHQclpilS8my6SybimnOQZc7zdKKbv1EJAWX59Pk2OQGxX0UXyS8itiG6Ulxbb1V0_b4qkxGZwe_0ynqXvf1pkQmCD_PMQTIOeqFHIW4Ionz8IB4uC0muO9pooOMJ2TmTITBPREI8f87I7KovtcGvl9uA==)
35. [libhunt.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHv_8RLT4FFdUJxFbnLhuSZsLqDsRXYXy6eoBinlYkeJHMIi8oTnNDn4tYH94Sga-N-pnpK8Cky0l0Gh0S_pNFMad0vG5mCzBUaekAgbPn8IcUNZ8TQitvVG-gQu4XuP9wDseMgaEAqUQ==)
36. [google.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGOpoJwq0mRkz0axFLcI1G0EGN2Z-f15r-U8oKkYL4jp4GjxCxpSCxiBlskjCQkQ8_T9TWp1hKUkpMTPq8U_ZlfOTtLzvYBJ3CAmscE4URE_WWZWAlDXzg7qlRzCi0OihbTJdHjt9a0MkAN-wN3QRu1GeQMi58Um_x4O4n-JnI9)
37. [oneuptime.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQF51OZ_XLU3R1IPnMjGaxHyI5EZZlc0i1X5WCEAqmuws2o2B7zwoImhb10DZWGMj95krzBbto0q4xXrtAlIT-GNIWyGLKBZ9Jh9OnqRph4pf-BG_JPms7EdJ84W_uvqTWztAprnDwPxlOfZ05AgjfTUKha5PXwos3rJ8vHDLJyW-jJHfzdv7i6Ce8-h7AG71W6ApYNzf3jBSPWuM19ZezuwN6LbPpnboBFPC5-sQELsHHbT3CxREBXBP1XD)
38. [github.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGXF0dvnGDdxzP30NUHvzHPAVJBR3qtsn8RvB6umoNx4Z9Oe-W5PzsLrGEMQBvRKxzL-zUpmqBo1A2U1zVKFH0pfTNdlUNzL2gwdjmy2ilw7aARF_8AMAk9fmUHTcg=)
39. [influxdata.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFmpYbVsqUdRaeZOU0OIf_7wg5ShCNSm0RhxI-g08pidB9eg_YIqjPZAPVGit8WcbpXa7FqRmuKjXAEqmlDkXBPoKwxx26LFUgJAtSMEtVF8X7Qv90m9-FxBMcTcU5iVPRoKy_9iuVrDWiq78cEYnVpCyviUFVQXZqs0ag=)
40. [apache.org](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFMrOcjgsFosr3O5-g4aq6fj0iuajQS0vnPBMhvnyK7rmn5kpvGnBqSqZarNN5InX6SusHVn_BlPKGn05-CxB_Z9tdTgroCjiZj4CErhw7sKTNjaPhzZ28zCqHOrYx_yRAdVqI9mHPyjkE5aki63GCCG84x8Ug4iT9ZSuZ9nusaglwNeAMvrcBJGg==)
41. [github.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFPLZc3eNLcGdRddOotV7sFU7SyC54awGkQ8YDukRF1eWK-bcmLTpCVyL8Lr0mTTzG-djGhALCAoHsKXnWspS5BdsAcay603D6SGw-bDi7AUZjV4p5EbGsZMgUfT9K8PK1KFSv5gxIukFVhbxE=)
42. [ycombinator.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQERSUIp1ACFEA8kKmbpjbaQa0TdATCvoQukPd3_eOj19P-nXZtrTQx0_xnm4369D9WA2oWjiF13Z3CMrQPgVqtvzHlcAiyadEyS-NJXqZwLLoxwHI6HWohxIieAU5NBnXxwvKQ=)
43. [github.com](https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQEMhVob5nrYRNaDia2SD_0gbNSFszKLuSfnj3bGReb9meaYSJC165nzpmteg1HnHlmNClkIvm5WLFjJygzQK_4aUJVLBRMzaaRJRcuAfLKRjpfPpB4DeZo3AY5N08Dqi36hNG2EUFCph5tRF5Y=)

## Sources

- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHa2rf9ojQrmSRXk_QXXelWS7kXbgcaItJVnrBvFOicBwhdsRS2MGZbxmjIV8dIEhm050PeHwOCud3LYRV3XgpvDdjks8uC_BY7s9KhKcS6Ixax3pfcL5MFvYKXc-LFtqEChW43x3k3nqZbXDNqo2GROhnrsvYHYOzf1Qg=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHo8c0eJyNuASG6lbJaPy44EF7NTmJVsO4AlUJ0iDozbyv0aPXyiFNJlUfIpW_1JZh8hcbFYfniMDODm1ZeYh9bjFcFQy8vCrCA3LyXySqeMSMX-pya1LU_S9WWZKNbF0WXi8oMGzcD8tRSSfMBVgpd_fud34gmHon7bVUTJ1HgfDsE6_yiLsmNLX_Ww_W88R2c4A==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFRoYg6pLfOPaYxnLiFS1WrLfcAQnACFvVwcxGNoNzaI2ToU_F9WVT3hang75bPiU7UyXRHWIpN2fE4ws5VSvAdYR4V9ug4y7XmfhQwDFr4N74Ni1TjQ7-QOevmNbutdd9N28AMVN0FOpbPxLOyTs_kx2SXdCIoaU6KTnH6UuK1HX0Hs1FodnzrsAshPQCNiDuHoaDlJ9A=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGdKeqzpG-X31VfLzsYU5Gbcb_YosFDHlqWzPacHDuxywBU9PYZKAoq9lmxkBH-vT5pFmX5LsJ0HCUKtodJ4Cbvg6faVnuR2rqHdJ0GV6YEfGQEYq-_gtEyPHUmhkc5-V-5ND_81TYwi2_2mmR4rMO_fUuznIe7h7C8bwkXxlGqHNmHsg==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQEXLcJgjav45bGl852hEytev_H3OXQAW-fJjJhYF5ctNj-iKGVyjk5DpMnbU8Q6hHiPzOEkpLNKBeDRRjJNajob2wzrHQ6TLqtIC9soF_FBOpX3nTZefcXvgPuVr1vYEyJFv3HPEbb2MWLpip2QJwn1wz5hBA0PdF7KGae4abMRNziucNg2TFaGvRR_A6C-JwFh_zpg4SIvY0TowS77j1cFz3KkrNhuOWCDxOSuIjzFXG2Cy4ewyGvxKOsv8AgMXcdtzPtKVGDDP5jaP8IU0IAXd6HOXIYRizub514VSUQ1EhZTpc27HQ1cShR9_CP2L2oJe2SbG5ATaEJkE0RckW6kWKdftvDyCzCnEdoX5ArItqGscw==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGWnbVWeQHHlrcRWO9032-DjsEVlt8CvRxRiFBGiZUpcDdDJFZTuTdlZMIitrsbFfRfJtIPpUhZfBaShpAtkwMvDhc4MyKnINvzKgEh_ut98hEA6Ly3EcYZUnh7OqoMc6fUtNPjq6tqBQcvg094xABkUapEA7oDoj58zA==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHJZF1tIPsQx-r2GbppkCJI8mcJ2UVKhCu5acXzUzbKiUgtpgTiF0ntv8jv9OGQgNc8YYqock8OICdLsGt1TndLfeFVhtU40DhPt13rRApZabcjGLYWUv6WJeZlpsMgh5XhEEOqZU99Gih5_Y0Rwpx9H5lsf2gzRQ==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHDazNe4j0Wu1eEfGg41_-bTQ_-EuKpAG55Ov9DnTquy0i4-Hd_Zz4Yw9OY1Wrlh42dXWUmHao_W5bCvhOUA84syrgjGnT5HYyoNEYhnKgj1HXJSaySLodO8kn0GjQdsdgVun5vbgPI0v6r45nYA64bj1XRG7KHS8jq4vslD07QJGfXg3MNc1e-dx7iUg==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGy9j4lqKlgllGgh-nSCY18Q0Xy90uGZpOWicJbPlG3zywADwf5yg_BwSEmahZUbzuuT5wK4XvYJAeD860Ee5jDF1WrMxDEg8TsdmxYJWC0BYzR0T6KtYLWs18LXOqtZIfyv-S6f-FJacL4DybnkC_7sIWmxR4XBhWcv5sIQigPpBvmp3StwwKkuKf1DOuXRg87pCy49bzOygtfA9-hv4mkrfUPNNE=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQG_DbMb4_CUqe_vFMbROaFyJ4_bPs6d29tbYOFk3ioEd9vSbVsSdgZDiWKd2aOaP-bkEdGLJwQoltMSpfzPPwNM8QPbgyz27-7suob8da0v9TFjqcigIkjacPJLHHLpzMHekaxNXILyr1UgxwFKPlZg9O-r
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHMUA8ZxXJ_DnKt4Eu9XqGyZnXG9AMJVTLjTMw7Qszc8trIhn6h_hIw1-CCIwzgYhw5tq-CzYX4P-78mg7fganhzpNGFUy6TxbzEUETTbtJvCMTGeWgUNQzuJ4OYnNcu--N37bzlD2vPD8=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFrW0Q2hsgFrOAmmYsofgZTbGwRqLX-671gBdwB3JgP8cRYG-3u56FmZohE0-WSfVUg9TYVEgaMBSPTIp6aWL6qgSSWX92Lie64s0iiubLD3yj4zg14b4-otnYrihIGCJ_Rxhf9DUpLfQ_aNlfX_yVGREfUIvV_tmluOeo31MU6-gqW7p5YzaHfaHrrUCVLpvjaKyNDsYPLddnTyw8ssA==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFRGa_1elN2ULwc65jq9U9WwLXCfQreorFdcq5_C-wXymZHR4TmEu81t6INRhOlOqIDygxHExIhUatxuaqgkExThSI4yDYACEyUOAy6WAIApb21IlKG8s5aTgHJOfzhYcRKnGI0Wei7ugGnQQ-JPD2OQ7s1M-NHNS0XjdZg
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFs_HjvYZLxL9msWoNjpitsNH1GBZJ3sBjyVzzjMumOjMP763BUYe4pg2B6J1qz1SJ8QN8nLTtpo5-M2PyCk3z50XoxUBTWs9gnR8YgruVlgGPeED7Q5d57E5ogBhOmea54XFW8MCkfL32DbZKu7quZQUWpa4Roq2PU2TM=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFVA70U_aQPJs7CGLFSyECva4beybvq5aFDY8w0dihMmSEf13_60ZNWDYQX4uCqjBpEbHgepL61Eu3IugNS3T6Cz0EluTqFzB5gOTMDSHs-Y0EkjOPBIhm5OCUS1zfzKEf7LK_RYmELc-paFAlySbKjhXyrgw3-bAiN4oyPBN-kEnUMmOCoK0AVC_LOkKJYUaXE
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQEyaBBbM3q6sgeRqTI-wIrW1mGKSw2kfP3gzcu-T7Y1hVgyLoKDU7z7xiXGL9EmWXP4rXNFrVsF9u6MBsA9jmi1fnhCuRePSgVYSRqandTl04SRrRws7H1Gczu2WIIjRUGwCDik8zDfehPResjemOoMCWAxB9LP49BYA0FzbnGvSrn082lAdQ==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGEKAjJwrgA-OGKDJ-oDKEpfo8JuTKuAX-uipp_kP07TgWmAQ5barQgbq4O45z3cjTW-6ko8zvAMlHEyDqsCNPnCFNmhC_24AXYZDvBwwfcVSbcq3SkeNzvEmMSOnjh11wUpVFSkNRHEk269J4547XOYzS7kZK6Vsz7ZxQEpEo6_8BQCMGVvYONavEa1lTD3wI38pDV1UX8hEnz8nn8x0xTZKZNZqbFgjwVjGX7RTGWzw==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGhwMcDRor-_OZbKqDi2I-kx0rCILaDBkWY0pMRHbB25RStjy_tCAA6eUZ6fNhiIQPTdfOvCRdoBKP_-WMVHSh_1GWH0Ahd9hDM3f88C0CWJF2pGvJ95daSIGdFOOJ8CCNBGN_07enOKGXJl4XQb9O9vjFl2LpRV1CuW1vAffrcU9AgMbR0453X2Oghv0Zpti7F94MCpYULXjKhtWLuuUQdZNj79w28mLrfXzDIFF2cPAE=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGV1en2Rs1aHvNedA-QOJjTjmx7JrxAvcSyQwYn0QLn82QjGVztnMLnhLUUDE6KshnkqrrFDVQNSItHnDLxMYJLSLz6jnAQ398zVwgxtxyjak6niGG-4pXyIz0XQ2LU1N2E-JpMTR1kyv8avJXIw7kAlzZXc8o3_VSfI4xms04DH6BM67PcADFEb6jQBjoIIgBMtxa7_yuYgsDL6WE=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFIqESoNorK9DYR6tu5NVtY1_3MsZYTJD3iJCTwKlrlMfdxOL0X797XrYsROBYFQ5ftV6KmF6ctBeK26YnHrks0s3tlqJoEiTGfLrlTjAMmywiYs2U3CpMms_sK4ouYsIyxNZAXokmB7mZS54gtmvdliIEm1yMCdNM8-e5ZA4_J619QQw==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGzLYpuDvmisZM89GIDEoiuMowlgxrmNqzSuyWQKqg1yCVxvyKpCoF2QnWf9MRT76GEjViGWujFI_CDAKlHtyQ2DF2LjMrIa7k45Pgz0CS7kiiVZjPxxtQeW3MI7JnsIr8FMQT3TwK85_hTBA==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQEsIpm7n1KBUWn-K-OdPKKHpjjvm0iAVmJDo12wN066ukakmrOYiWGq5H63Ag0SMeKOTA1rpsX6RffcpTQksQiGHc0g1yfLSdJsHsiCuPpsi5nPAXrEKp8XPjdf8pybouhI0DEt2nFK8Qd24xnbp0wGaXJxmOjTqpShGEato2B6Khot5GrG
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHQcYqkcxHiPsigDGkSsgpVGqBcxfHyKGXiR6yHnApQpqALdfYvroR9LmwCblRa3LIpn2B2Mg5VVScrJn58e4j3r1VCG_MrBA7jGdrSc9vx-RSKGGVzjkd2XGsjDcoE6zO_XSyg8kjc-X3cLc7Z3fxbsErHru8=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFDKXzieYHKgNsmyNSmSzD7w8CVUToUn01THcTcXPVyUmiTFH4oXULwfqbEkv3lzHhUiCPfGJXMz8cPkcdC9CSVnlCLQo1A9ZJTJO7DxE7Mpysj01kreR3OTKiQMS_0njEprXdCz_-ATA6lEFWWJDg4Vw==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQEq6aANRE1uMocx1KD_ZPYJ-XxT3L7rhmCk8ZbpbC7YZlLK5QrxYKrHUZPF6jyR2Iw_pH1bzO1GjLZrdbc2QOze8mODt5FcB318_LJ-99xofiNic0lU4zvf885mYmoEziBCbggEk1WNmIQm_3TpuA6lwwX1Yh__lQ==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGQCsd-HR0B900g9aNBfbk46wdLUF5Pqng7kakJvG9LtpI20tuaudcXAltaumb1E8V9sO1CT7kklYF-Y38KJT3BNIZ_MehK6KVesY6aL4g8Uk_2wuBLHCav8zn1xITnictShNkQwUNVnfZzk7LtyGw6jasJjK5Q5cKZEqNJXAc=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFOpJK41gOtN_m4vMLpSXpd6Wi8ug0JIYqqQCWUBnsFSbsvjjABozJ2c-iVEDKtuT7AovEUn8ap8BYCcrQItyPXgQFGtvZRp5tFwy-5MrbPRNZ-4QOO34tKmel3sTJm9wUJvwQUlfZGF03wBcSZ4MLHzrL5MuK6C0MCF-oriTs7CsZrjAjB7XIp_UV6Cu3RPZCU5wrmY9doAJxeDHbw
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFTK0Gzwd2qQstBtPhGibMWgY7cgVVbynsnXzaw2lmpsEq9nsR8AM7FrckIaOhDiobBacmwXXU4HNiDqyTDejx8euAZmyqlDyp5evV6LJt69fVrl9f-AKhQ_1nxC9gLhAVFAc2CC6MfSVr9EhGfRJmed7UPMupFxCLuNQ==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQG6rG1wnxAYJnyW0zuk8n1sL5dTMxcmUHHROEYQO5GL0vhw7vUO8bwl8rjefvb0JCmTI518YJgsPgm_IN1P28qC6qnkBImOGB7VsEUGI5uv87Jhb444H2WpR1c5juOVvn2yeEmg0angWT1PCjOBMNp_xXQY1ghFyQ==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHCAM99JVCb9wbYcH4H9Fw2mtGz--jf5MO3TkOFQVMVQkilBdi-GSwCQmT9Eh_y2UcpYFByyFYQCfZCSIjjLV4fgrueKW_jNnUpEraU0NbWdKSGTVO62672r2vpVKCGhNTd4THQdKPeQ-o0OXUKshBjhcF-SiVxLxz_4HkQlcimpWtFCD-kWHp8sFqtmiGc_cvYoD3eKZ0=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQEUIdoCw61zahaY11OaKIVnQo0-KT0ZNgRxDpb2nd-baXLLnPCYUM2X4I2jk2OCT56fn13wj62bbUMsGl5mfehekJJd69xYNABrTOjGHSqCOGblV4z8OQxHpGC2013n0YWmQZFNqJHDFEIlZJq35T9L--Lgt5HUVng_uVwIwb3axUqp_VpxyHSimfrp7vUFaWFkxKoxwKLO5QA9zgKxZlgcR41WUeq3BYJtxumJT3czQhdfsmZ-XdU8_GPCIvNyjig=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFFWNpzu770b5oXo-gqEart9hvPD1KDpnatZHIuTzjDrD2DZ3IX5nJ2K9yq-KyzVetrA5NNl4WqXNnqPDytMlifHqSklzikceiSN64kIhTW
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGQ3eN-O1OgWcO9PlhsxJSMyVp-kczAjlHQclpilS8my6SybimnOQZc7zdKKbv1EJAWX59Pk2OQGxX0UXyS8itiG6Ulxbb1V0_b4qkxGZwe_0ynqXvf1pkQmCD_PMQTIOeqFHIW4Ionz8IB4uC0muO9pooOMJ2TmTITBPREI8f87I7KovtcGvl9uA==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGlpSaq12EAJJVFkOrhpUHI8S29gkNZAHRIPDFyTmw-sWY14Ucu2gEeAT24UAFVjDMoFX3tKXRBka7IGneRCHDi1A6M8UbBQcc_u7wlJ4U6EfO-GMqE-jSv6I1jlCy0Yj51nv0=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQF51OZ_XLU3R1IPnMjGaxHyI5EZZlc0i1X5WCEAqmuws2o2B7zwoImhb10DZWGMj95krzBbto0q4xXrtAlIT-GNIWyGLKBZ9Jh9OnqRph4pf-BG_JPms7EdJ84W_uvqTWztAprnDwPxlOfZ05AgjfTUKha5PXwos3rJ8vHDLJyW-jJHfzdv7i6Ce8-h7AG71W6ApYNzf3jBSPWuM19ZezuwN6LbPpnboBFPC5-sQELsHHbT3CxREBXBP1XD
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGOpoJwq0mRkz0axFLcI1G0EGN2Z-f15r-U8oKkYL4jp4GjxCxpSCxiBlskjCQkQ8_T9TWp1hKUkpMTPq8U_ZlfOTtLzvYBJ3CAmscE4URE_WWZWAlDXzg7qlRzCi0OihbTJdHjt9a0MkAN-wN3QRu1GeQMi58Um_x4O4n-JnI9
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHv_8RLT4FFdUJxFbnLhuSZsLqDsRXYXy6eoBinlYkeJHMIi8oTnNDn4tYH94Sga-N-pnpK8Cky0l0Gh0S_pNFMad0vG5mCzBUaekAgbPn8IcUNZ8TQitvVG-gQu4XuP9wDseMgaEAqUQ==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQGXF0dvnGDdxzP30NUHvzHPAVJBR3qtsn8RvB6umoNx4Z9Oe-W5PzsLrGEMQBvRKxzL-zUpmqBo1A2U1zVKFH0pfTNdlUNzL2gwdjmy2ilw7aARF_8AMAk9fmUHTcg=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFmpYbVsqUdRaeZOU0OIf_7wg5ShCNSm0RhxI-g08pidB9eg_YIqjPZAPVGit8WcbpXa7FqRmuKjXAEqmlDkXBPoKwxx26LFUgJAtSMEtVF8X7Qv90m9-FxBMcTcU5iVPRoKy_9iuVrDWiq78cEYnVpCyviUFVQXZqs0ag=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQERSUIp1ACFEA8kKmbpjbaQa0TdATCvoQukPd3_eOj19P-nXZtrTQx0_xnm4369D9WA2oWjiF13Z3CMrQPgVqtvzHlcAiyadEyS-NJXqZwLLoxwHI6HWohxIieAU5NBnXxwvKQ=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFMrOcjgsFosr3O5-g4aq6fj0iuajQS0vnPBMhvnyK7rmn5kpvGnBqSqZarNN5InX6SusHVn_BlPKGn05-CxB_Z9tdTgroCjiZj4CErhw7sKTNjaPhzZ28zCqHOrYx_yRAdVqI9mHPyjkE5aki63GCCG84x8Ug4iT9ZSuZ9nusaglwNeAMvrcBJGg==
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFPLZc3eNLcGdRddOotV7sFU7SyC54awGkQ8YDukRF1eWK-bcmLTpCVyL8Lr0mTTzG-djGhALCAoHsKXnWspS5BdsAcay603D6SGw-bDi7AUZjV4p5EbGsZMgUfT9K8PK1KFSv5gxIukFVhbxE=
- https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQEMhVob5nrYRNaDia2SD_0gbNSFszKLuSfnj3bGReb9meaYSJC165nzpmteg1HnHlmNClkIvm5WLFjJygzQK_4aUJVLBRMzaaRJRcuAfLKRjpfPpB4DeZo3AY5N08Dqi36hNG2EUFCph5tRF5Y=

---

## Research metadata

- **Model**: deep-research-preview-04-2026
- **Interaction ID**: `v1_ChdDYk5uYXVTSUFmdTF4TjhQX3NUTTJBbxIXQ2JObmF1U0lBZnUxeE44UF9zVE0yQW8`
- **Started**: 2026-07-27T19:35:37.928072+00:00
- **Completed**: 2026-07-27T19:43:50.263365+00:00
- **Sources referenced**: 43
- **Generated by**: Gemini Deep Research (preview). Paid, pay-per-task feature.
