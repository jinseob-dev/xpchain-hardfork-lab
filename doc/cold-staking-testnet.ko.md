# 콜드 스테이킹 테스트넷 가이드

[English guide](cold-staking-testnet.md)

콜드 스테이킹은 자금을 출금할 수 있는 소유자 키와 온라인 노드가 사용하는
스테이킹 키를 분리합니다. 테스트넷 단계에서는 모든 보상이 동일한 콜드
스테이킹 계약으로 돌아가며, 노드 운영자 수수료 기능은 의도적으로 비활성화되어
있습니다.

아래 지갑 정책은 합의 규칙의 최소 스테이킹 나이와 기존 보상 공식을 변경하지
않습니다.

- `-coldstaketargetage=<일>`: 위임 출력의 권장 스테이킹 나이입니다. 기본값은
  `32`일이며, `0`은 해당 네트워크의 합의 최소 나이를 사용합니다.
- `-coldstakefallbackage=<일>`: 체인이 정체됐을 때 적용할 정책상 하한입니다.
  기본값은 `3`일이며 합의 최소 나이보다 짧아질 수 없습니다.
- `-coldstakefallbackdelay=<분>`: 체인 팁이 이 시간 동안 갱신되지 않으면 권장
  나이를 정책상 하한으로 낮춥니다. 기본값은 `15`분입니다.

## 테스트넷 진행 절차

소유자 지갑과 스테이킹 노드 지갑에 동일한 계약을 생성하거나 가져옵니다.

```text
createcoldstakingaddress "소유자_주소" "스테이커_주소"
```

위임 금액에 따른 자동 분할 계획을 먼저 확인한 다음 위임 거래를 생성합니다.

```text
estimatecoldstakingsplit 1000
delegatecoldstaking "계약_주소" 1000
```

두 명령은 선택 사항 객체로 `minimum_age_days`, `target_age_days`,
`maximum_age_days`, `target_probability`, `maximum_outputs`,
`minimum_output_amount`를 지원합니다. 부동소수점 계산은 합의 규칙이 아닌 지갑의
분할 정책에서만 사용됩니다. 실제 거래 금액은 모두 정수 사토시이며, 분할 출력의
합은 요청한 위임 금액과 정확히 같습니다.

계약 출력과 현재 지갑의 역할을 확인합니다.

```text
listcoldstaking
```

소유자 지갑에서 위임 자금을 회수합니다. 수수료는 동일한 계약으로 반환되는
거스름돈에서 차감되므로, 요청 금액 외에 수수료를 지불할 계약 잔액이 남아 있어야
합니다.

```text
withdrawcoldstaking "계약_주소" "수령_주소" 10
```

짧은 regtest에서는 스테이킹 노드에 `-coldstaketargetage=0`을 사용합니다. 이
옵션은 로컬 후보 선택 정책만 바꾸며, 합의 검증은 항상 네트워크 최소 나이를
강제합니다.

## 자동 안전성 검사

`feature_pos_staking.py`는 서로 분리된 소유자 노드와 스테이커 노드로 위임
스테이킹의 전체 생명주기를 검사합니다. 탭루트 스테이킹, 보상 귀속, P2P 전파,
재인덱싱 검증 외에도 다음 조건을 확인합니다.

- 소유자 지갑은 `owner=true, staker=false`로 표시됩니다.
- 스테이커 지갑은 `owner=false, staker=true`로 표시됩니다.
- 스테이커 지갑의 `withdrawcoldstaking` 호출은 거부됩니다.
- 소유자의 출금은 빈 값(`OP_0`)인 소유자 분기 선택자를 사용하고 블록에
  확정됩니다.
- 지갑 파일 백업을 복원하면 콜드 스테이킹 계약과 소유자 권한도 복구됩니다.

실행 명령은 다음과 같습니다.

```text
python3 test/functional/test_runner.py feature_pos_staking.py
```

## 공개 테스트넷 조건

공개 테스트넷의 주요 네트워크 조건은 다음과 같습니다.

- P2P 포트: `18798`
- Bech32 주소 접두사: `txpc`
- 합의 최소 스테이킹 나이: `1시간`
- PoW 부트스트랩 구간: 높이 `1~200`
- PoS 시작 높이: `201`
- 탭루트와 콜드 스테이킹: 제네시스부터 활성화
- 메인넷 주소 접두사 `xpc` 및 regtest 접두사 `xpcrt`와 분리
- 기존 XPC 테스트넷과 다른 제네시스 및 네트워크 매직 사용

메인넷 지갑 키나 데이터 폴더를 테스트넷에서 재사용해서는 안 됩니다.

## 참여자용 설치 패키지

프리뷰 릴리스는 다음 네 플랫폼을 대상으로 빌드합니다. 모든 빌드 작업은 산출물의
실제 CPU 아키텍처를 검사한 뒤 패키징해야 합니다.

- Apple Silicon macOS: `*-testnet-macos-arm64.dmg`
- Windows Intel/AMD 64비트: `*-win64-setup.exe` 또는 `*-win64.zip`
- Linux ARM64: `*-linux-arm64.tar.gz`
- Linux Intel/AMD 64비트: `*-linux-x86_64.tar.gz`

macOS에서는 `XPChain-Testnet.app`, Windows에서는 시작 메뉴의 `XPChain Core
(testnet, 64-bit)`, Linux에서는 `xpchain-testnet`을 실행합니다. 이 실행 항목들은
자동으로 `-testnet`을 적용하고 운영체제별 테스트넷 전용 데이터 폴더를 사용합니다.

- macOS: `~/Library/Application Support/XPChain-Testnet`
- Windows: `%APPDATA%\XPChain-Testnet`
- Linux: `~/.xpchain-testnet`

설치 파일의 체크섬은 릴리스에 포함된 `SHA256SUMS.txt`와 반드시 비교해야 합니다.
현재 프리뷰 빌드는 임시 서명이므로, 정식 배포 전에는 Apple Developer ID 서명과
공증 및 Windows Authenticode 서명이 별도로 필요합니다.

프리뷰 클라이언트에는 현재 하드포크 실험망 부트스트랩 노드 2대의 IP가 숫자
시드로 포함됩니다. 필요하면 다음 옵션으로 직접 연결할 수도 있습니다.

```text
-addnode=<부트스트랩-IP>:18798
```

각 참여 노드에서 다음 RPC를 확인합니다.

```text
getconnectioncount
getblockchaininfo
getpeerinfo
```

테스트 코인을 지급하거나 위임하기 전에 아래 준비 조건을 모두 만족해야 합니다.

1. 서로 독립된 피어가 2개 이상 연결되어 있어야 합니다.
2. 모든 참여 노드의 최고 블록 높이와 해시가 같아야 합니다.
3. `initialblockdownload=false`여야 합니다.
4. 모든 노드의 `chain` 값이 `test`여야 합니다.
5. 거래소·메인넷용 지갑과 데이터 폴더가 완전히 분리되어야 합니다.
6. 일반 검증 노드는 `minting=false`로 시작해야 합니다.
7. 방화벽은 테스트넷 P2P 포트 `18798`만 필요한 대상에 공개하고 RPC 포트는
   인터넷에 공개하지 않아야 합니다.

## 새 실험망 시작 시 주의사항

이 테스트넷은 기존 XPC 테스트넷 및 `preview-testnet-v0.1`과 호환되지 않는 새
제네시스 블록과 네트워크 매직을 사용합니다. 모든 참여자는 이전 프리뷰 노드를
종료하고 테스트넷 체인 데이터만 초기화한 후 동일한 새 버전으로 시작해야 합니다.
지갑을 초기화하기 전에는 반드시 별도로 백업합니다. 기존
`seed1`/`seed2`/`seed3.xpchain.co.kr` DNS 시드는 이 실험망에서 사용하지 않습니다.

부트스트랩 노드 1대가 높이 200까지 PoW 블록을 만든 뒤 테스트 코인을 배포하고,
최소 1시간이 지난 높이 201부터 PoS 및 콜드 스테이킹 검증을 시작합니다.

## 공개 테스트넷 통과 기준

인프라 구축 후 다음 조건을 모두 확인해야 공개 테스트넷 검증을 통과한 것으로
판정합니다.

1. 두 개 이상의 독립 노드가 24시간 이상 동일한 체인 팁을 유지합니다.
2. 소유자 지갑을 종료한 상태에서 스테이커 노드가 콜드 PoS 블록을 생성합니다.
3. P2TR 주소의 PoS 블록을 모든 검증 노드가 수락합니다.
4. 스테이커 지갑의 출금 시도가 항상 거부됩니다.
5. 소유자 지갑의 부분·전체 출금이 정상 확정됩니다.
6. 노드 재시작, 네트워크 단절·재연결, `-reindex` 이후 체인 팁이 일치합니다.
7. 소유자 지갑 백업 복원 후 계약과 출금 권한이 정상 복구됩니다.
8. 잘못된 콜드 스테이킹 및 탭루트 블록을 모든 노드가 동일하게 거부합니다.
