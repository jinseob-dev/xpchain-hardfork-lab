# 하드포크 전 개선 사항 (선행 조건 정리)

목표 하드포크 페이로드는 **Taproot 스테이킹**과 **콜드 스테이킹**이다
(`doc/architecture-separation-roadmap.md`의 P6 단계). 이 문서는 그 페이로드를
설계·활성화하기 **전에** 끝내야 하는 작업을 위험도 순으로 정리한다.

각 항목은 코드에서 확인한 사실만 담았다. 파일:줄 참조는 이 문서 작성 시점의
`master` 기준이다.

---

## 0. 현재 체인 상태 (2026-09 기준 실측)

| 항목 | 값 | 출처 |
|------|-----|------|
| 메인넷 높이 | **3,889,343** (2026-09-05 02:25 UTC) | `explorer.xpchain.co.kr/api/getblockcount` |
| 제네시스 | 2018-10-23 (`1540301656`) | `chainparams.cpp:142` |
| 블록 간격 | 목표 60초 / 실측 58.6~61.1초 (최근 10만/3만/1만 블록) | `chainparams.cpp:86` |
| PoW→PoS 전환 높이 | 10,275 | `chainparams.cpp:125` |
| **`TaprootHeight`** | **4,200,000 — 미도달, 약 310,000블록(7개월) 뒤** (§A-1로 변경, 이전 값 3,000,000) | `chainparams.cpp:83` |
| 연 보상률 | 5% (하한 도달, `525600 * 5 < 높이`) | `pos/reward.cpp:41` |
| 마지막 체크포인트 | 173,800 (2019-02경) | `chainparams.cpp` |
| `nMinimumChainWork` / `defaultAssumeValid` | `0x00` / `0x00` | `chainparams.cpp:120,123` |
| `chainTxData` | 2019-04-16 | `chainparams.cpp:177` |

### Taproot의 실제 상태: 이 빌드에서만 켜져 있다

`git log`로 확인한 바, Taproot 지원 **전체**가 단일 커밋에서 왔다.

```
16c5cb81d 2026-04-29 Fix Taproot transaction confirmation and consensus-level hashing bugs
```

이 커밋 하나에 인터프리터의 witness v1 분기, `SCRIPT_VERIFY_TAPROOT`,
`TaprootHeight = 3000000`, `DEPLOYMENT_TAPROOT`, 그리고
`DEFAULT_ADDRESS_TYPE = BECH32M`이 모두 들어 있다. 이 커밋을 포함하는 유일한 공개
릴리스는 `v0.27.0-rc1`(2026-09-03, 프리릴리스)이고 **전체 자산 다운로드 합계가 1건**,
나머지 두 태그(`preview-gui`, `preview-install`)는 이전에 draft였고, 이후 preview 워크플로는 공개 prerelease로 게시한다.

따라서 실제 상태는 다음과 같다.

| | 라이브 네트워크 | 이 저장소의 빌드 (미배포) |
|---|---|---|
| Taproot 코드 | **없음** | 있음 |
| witness v1 출력 | **누구나 쓸 수 있음** | 3,000,000 이상에서 Taproot 규칙 적용 |
| 지갑 기본 주소 | P2WPKH | **P2TR (bech32m)** |

즉 Taproot는 "이미 켜진 기능"이 아니라 **이 빌드에만 존재하는, 네트워크와 합의가
어긋난 상태**였다. `TaprootHeight = 3000000`은 커밋 작성 시점(당시 높이 약 3,703,000)에
이미 지난 값이었으므로, 이 빌드는 약 889,000개의 과거 블록에 Taproot 규칙을
**소급 적용**했다.

이 사실이 아래 §A-1의 성격을 바꾼다. 문제는 "스테이킹이 안 된다"가 아니라
**"네트워크가 그 출력을 누구나 쓸 수 있는 것으로 취급한다"**다.

§A-1의 수정으로 위 표의 오른쪽 열은 "4,200,000 이상에서 Taproot 규칙 적용 / 기본 주소는
P2WPKH"가 됐다. 소급 적용은 없어졌고, 활성화는 예정된 이벤트가 됐다.

---

## A. v0.27.0 정식 배포 전에 고쳐야 하는 것

이 구간은 하드포크 페이로드와 무관하고, **아직 배포량이 0인 지금이 무비용으로
고칠 수 있는 유일한 시점**이다. 배포 후에는 같은 수정이 조정된 하드포크를 요구한다.

### A-1. `TaprootHeight`를 미래로 옮기고, 그때까지 bech32m을 기본값에서 뺀다

> **상태: 코드 수정 완료.** 메인넷 `TaprootHeight = 4200000`(약 2027-04 도달 예상), 기본 주소
> 타입은 `BECH32`로 복귀, 활성화 전 bech32m 발급은 거부. 아래 "할 일" 1·2번(활성화 높이 아래
> witness v1 전수 확인, 본인 P2TR 잔액 회수)은 동기화된 메인넷 노드가 필요해 남아 있다.

두 값이 함께 문제를 만든다.

```83:83:src/chainparams.cpp
        consensus.TaprootHeight = 3000000;
```

```88:88:src/wallet/wallet.h
constexpr OutputType DEFAULT_ADDRESS_TYPE{OutputType::BECH32M};
```

`getnewaddress`를 옵션 없이 부르면 Taproot(bech32m) 주소가 나온다. 그런데 라이브
네트워크에는 Taproot 코드가 없으므로(§0), 그 노드들은 witness v1 출력을 소프트포크
업그레이드 훅으로 보고 **무조건 성공 처리**한다:

```1665:1670:src/script/interpreter.cpp
    } else if (flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM) {
        return set_error(serror, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM);
    } else {
        // Higher version witness scripts return true for future softfork compatibility
        return set_success(serror);
    }
```

`SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM`은 멤풀 정책에만 있고
(`policy/policy.h:67`) 블록 검증 플래그에는 없다(`validation.cpp` `GetBlockScriptFlags`).
즉 **활성화 전 witness v1 출력은 합의 수준에서 누구나 쓸 수 있다.** 스테이킹이 안
되는 것(§B-3)은 그다음 문제다.

여기서 나오는 위험이 두 가지다.

1. **미조정 하드포크.** `TaprootHeight`를 3,000,000(과거)으로 둔 채 v0.27.0을
   배포하면, 누군가 P2TR 출력을 쓰는 순간 구 노드는 anyone-can-spend 스윕을
   수락하고 신 노드는 거부한다 → 체인 분기. 활성화 높이를 미래로 옮기면 이 사고가
   **예정된 활성화**로 바뀐다.
2. **검증되지 않은 소급 적용.** 지금 설정은 과거 889,000블록에 Taproot 규칙을 소급
   적용한다. 그 구간에 Taproot 규칙으로 실패하는 witness v1 지출이 하나라도 있으면
   신 빌드는 그 블록을 넘어 동기화하지 못한다. 익스플로러 API로 최근 약 180,000블록
   범위를 표본 조사(26블록/72tx)했을 때 witness v1 출력은 0건이었고 전부
   `witness_v0_keyhash`와 `nulldata`였다. 표본이 작으므로 근거는 약하다.

배포량이 사실상 0이라는 점(§0)이 이 작업을 **지금 하면 안전하게** 만든다. 제3자가
보유한 P2TR 출력이 없으므로 고립될 자금이 없다.

**할 일 (순서대로)**

1. **[남음 — 노드 필요]** 활성화 높이 **아래에 witness v1 출력이 하나도 없음을 전수
   확인**한다(표본이 아니라 전 구간). 있으면 그 지출이 Taproot 규칙을 통과하는지 개별
   판정한다. `contrib/devtools/scan-witness-programs.py`가 동기화된 노드의 RPC로 전
   구간을 훑는다.
2. **[남음 — 노드 필요]** 자신이 테스트로 만든 P2TR 잔액이 있으면 P2WPKH로 회수한다.
   활성화 전까지 그 출력은 네트워크에서 보호되지 않는다.
3. **[완료]** `TaprootHeight`(메인넷)를 미래 높이로 설정했다. 실측 블록 간격은 최근
   1만/3만/10만 블록에서 각각 61.1초 / 59.8초 / 58.6초이므로 하루 약 1,440블록이다.
   높이 3,889,343(2026-09-05)에서 **4,200,000**은 약 310,000블록 뒤, 약 7개월 후다.
   유예 자체보다 §C-1의 묶음 활성화(Taproot + Taproot 스테이킹 + 콜드 스테이킹)를
   담을 수 있는 폭을 기준으로 골랐다. 테스트넷·regtest는 `0`으로 유지해 테스트가 계속
   Taproot 경로를 타게 한다.
4. **[완료]** `DEFAULT_ADDRESS_TYPE`을 `BECH32`(P2WPKH)로 되돌렸다. 활성화 여부는
   `TaprootOutputsProtected()`가 팁 기준으로 판정한다. 명시적으로 요청한 bech32m은
   활성화 높이를 담은 오류로 **거부**하고, `-addresstype`/`-changetype`에서 온 bech32m은
   bech32로 대체한 뒤 시작 시 경고한다. 위험이 "스테이킹 불가"가 아니라 "누구나 쓸 수
   있음"이므로 경고보다 거부를 골랐다. GUI는 활성화 전까지 Bech32m 항목을 감춘다.
   막은 경로: `getnewaddress`, `getrawchangeaddress`, `TransactionChangeType()`의 잔돈,
   `fundrawtransaction`/`walletcreatefundedpsbt`의 `change_type`, GUI 수신 화면,
   `paymentserver`의 환불 주소. `addmultisigaddress`/`createmultisig`는 스크립트에 대한
   bech32m이 P2WSH로 폴백하므로 가드가 필요 없다.
5. **[완료]** `-addresstype` / `-changetype` 도움말에 `bech32m`을 추가했다.

**로드맵에 미치는 영향:** Taproot가 아직 네트워크에서 활성이 아니므로, **Taproot
활성화와 Taproot 스테이킹(§D-4)을 하나의 조정된 활성화로 묶을 수 있다.** 계획했던
하드포크 한 번이 줄어든다. 콜드 스테이킹(§D-3)도 같은 활성화에 태울 수 있다.

### A-2. 스테이킹 불가·미보호 잔액이 보이지 않는다

지갑은 "스테이킹 가능한 잔액"과 그렇지 않은 잔액을 구분해 보여주지 않는다. A-1을
고쳐도 이미 Taproot 주소로 받아둔 코인은 그대로 남는다. 활성화 높이 이전이라면 그
잔액은 스테이킹이 안 되는 것에 그치지 않고 **네트워크에서 보호되지 않는다**. 사용자가
스스로 회수(자기 자신에게 P2WPKH로 재전송)할 수 있게 하려면 먼저 보여야 한다.

**할 일**

1. `getstakinginfo`(또는 `getwalletinfo`)에 스테이킹 불가 금액과 그 이유를 노출한다.
   활성화 높이 이전의 P2TR 잔액은 별도로, 더 강한 문구로 구분한다.
2. GUI 개요 화면에 같은 정보를 표시하고, 자기 자신에게 통합 전송하는 안내를 제공한다.
3. 민터가 UTXO를 건너뛸 때 이유를 집계해 주기적으로 한 번만 로그한다(현재는 매 루프
   조용히 `continue`).

---

## B. 하드포크 전 필수 — 합의 분기 위험

### B-1. 보상 계산이 부동소수점이고, 릴리스가 다섯 개 플랫폼으로 나간다

> **결정: 이번 하드포크에서 제외.** 기존 메인넷과 1사토시까지 동일하다는 다중 플랫폼
> 증명이 완료되기 전에는 보상 공식과 연산 순서를 변경하지 않는다. 정수화는 별도 제안으로
> 유지한다.

```69:74:src/pos/reward.cpp
    CAmount annual = nAmount * GetAnnualRate(nHeight, consensusParams);

    double_t coefficient = dRewardCurveMaximum / (1.0 + (dRewardCurveMaximum / dRewardCurveBase - 1.0) * exp(-dRewardCurveSteepness * nTime));
    coefficient = std::min(coefficient, dRewardCurveLimit);

    return (CAmount) (annual * coefficient * nTime / (365 * 24 * 60 * 60));
```

이 값은 `ConnectBlock`에서 `blockReward`가 되고, 블록이 그보다 많이 지불하면
거부된다(`validation.cpp:2149`). 따라서 **노드 간에 1사토시라도 결과가 다르면 체인이
갈라진다.**

위험의 크기를 측정했다. 계수(`coefficient`)에 1 ULP 차이만 나도 최종 절단 결과가
바뀌는 비율:

| 조건 | 결과 |
|------|------|
| 무작위 (금액 1~5,000,000 XPC, 나이 3~60일) 20만 건 | **0.038%** (약 2,600건 중 1건) |
| 10,000,000 XPC / 60일에서 중간값 크기 | `2.59e20` — `2^53 ≈ 9.0e15`를 훨씬 초과 |

중간 계산이 `2^53`을 넘어 정밀도를 잃는다는 점이 문제를 키운다. `annual * coefficient
* nTime`이 먼저 계산되고 나눗셈이 마지막에 오기 때문이다.

노출이 최근 커졌다. `release.yml`은 이제 **다섯 개 타깃**을 빌드한다:

| 타깃 | libm / FPU |
|------|-----------|
| Linux x86_64 | glibc, SSE2 |
| Windows x86_64 (mingw) | mingw/MSVCRT, SSE2 |
| **Windows x86 (i686)** | mingw/MSVCRT, **x87 80비트 레지스터** |
| macOS arm64 | Apple libm, NEON (FMA 축약) |
| macOS x86_64 | Apple libm, SSE2 |

`exp()`는 IEEE-754가 정확한 반올림을 요구하지 않는 함수다. 여기에 i686 빌드는
`-mfpmath=sse`를 지정하지 않는다(`release.yml:154-158`). gcc의 32비트 x86 기본값은
`-mfpmath=387`이고 `-msse2`는 비활성이므로(`gcc -m32 -Q --help=target`으로 확인),
이 빌드는 `double` 중간값을 x87 80비트 레지스터에서 계산한다. 이는 64비트 빌드의
SSE2 경로보다 **정밀도가 높아** 절단 결과가 달라진다.

즉 서로 다른 `exp()` 구현 다섯 개와 서로 다른 부동소수점 레지스터 모델 세 개가 합의
값을 계산한다. 체인이 지금까지 갈라지지 않은 것은 다중 플랫폼 패키징이 **최근에
추가된** 것이기 때문이다(#24, #35, #36).

**할 일 (순서가 중요)**

1. 정수 전용 구현을 만든다. `exp()`는 고정소수점 급수 또는 사전 계산 룩업으로
   대체한다. `nTime`이 `nStakeMinAge..nStakeMaxAge` 범위의 정수이므로 테이블화가
   가능하다.
2. **차분 테스트로 동등성을 증명한다.** 전 구간(높이 × 금액 × 나이)에서 기존
   double 구현과 정수 구현의 출력을 비교한다. 특히 §A에서 측정한 경계 근처를
   집중적으로 본다.
3. **과거 체인 전체 재검증(리플레이)**으로 3,889,312 블록 전부에서 정수 구현이
   기존과 동일한 `blockReward`를 내는지 확인한다(§C-4).
4. 값이 완전히 일치하면 **하드포크 없이** 교체할 수 있다. 한 블록이라도 다르면 그
   블록 이후로만 새 규칙이 적용되도록 **하드포크에 태운다**.

### B-2. `CheckProofOfStakePure`가 검증 중인 블록이 아니라 현재 팁을 읽는다

> **상태: 완료** — `GetCoinStakeScriptFlags(nHeight, params)`로 분리하고 `ConnectBlock`이
> `pindex->nHeight`를 넘긴다. 전역 `Params()`를 읽던 `CheckProofOfStake` 오버로드도 제거했다.
> 단위 테스트 `coinstake_script_flags_follow_block_height`가 경계를 고정한다.

```76:79:src/pos/kernel.cpp
    unsigned int nFlags = SCRIPT_VERIFY_NONE;
    if (chainActive.Height() + 1 >= params.TaprootHeight) {
        nFlags |= SCRIPT_VERIFY_TAPROOT;
    }
```

같은 판단을 하는 다른 자리는 올바르게 `pindex->nHeight`를 쓴다:

```1815:1815:src/validation.cpp
    if (pindex->nHeight >= consensusparams.TaprootHeight) {
```

따라서 코인스테이크 서명 검증만 **현재 팁 기준**으로 스크립트 플래그를 정한다.
`VerifyDB` 레벨 4(과거 블록 재검증)나 재구성 중에는 `chainActive.Height()`가
`pindex->nHeight`와 다르므로, 같은 블록이 상황에 따라 다르게 판정될 수 있다.
`src/pos/` 이동 이전부터 있던 문제이고(구 `src/kernel.cpp:65`), 함수 이름이 `Pure`인
것과도 맞지 않는다.

**할 일** 검증 대상 블록의 높이를 인자로 받아 사용한다.

이 항목을 A-1과 같은 PR에 묶은 이유가 있다. `TaprootHeight`가 과거에 있던 동안은 메인넷
높이가 3,000,000을 훨씬 넘겨서 두 식이 팁에서 우연히 같은 값을 냈다. 활성화 높이가
미래가 되면 **활성화 경계를 지나는 동안 두 식이 실제로 다른 값을 낸다**
(`chainActive.Height() + 1`은 동기화 진행에 따라 움직이고 `pindex->nHeight`는
고정이다). A-1만 하면 이 버그가 발현하는 창이 열린다.

### B-3. 스테이킹 가능한 스크립트 타입이 함수마다 다르다

세 함수가 각각 다른 타입 집합을 허용한다.

| 스크립트 타입 | `IsCoinStakeTx`<br>(`IsDestinationSame`) | `GetPubKeysFromCoinStakeTx`<br>(블록 서명) | `EqualDestination`<br>(분할 보상 코인베이스) | `CWallet::SignReward`<br>(지갑) |
|---|---|---|---|---|
| P2PKH | ✅ | ✅ | ✅ | ✅ |
| P2WPKH | ✅ | ✅ | ✅ | ✅ |
| P2SH(P2WPKH) | ✅ | ✅ | ✅ | ✅ |
| P2PK | ✅ | ✅ | ❌ | ❌ (명시적 거부) |
| P2WSH | ✅ | ✅ | ❌ | ❌ (명시적 거부) |
| Multisig | ❌ (`sol.size() != 1`) | ✅ | ❌ | ❌ (명시적 거부) |
| **P2TR** | ✅ | ❌ (`default: return false`) | ❌ | ❌ (`GetKeyForDestination` 실패) |

참조: `pos/stake.cpp:22`(`IsDestinationSame`), `pos/stake.cpp:72`(`GetPubKeyFromScript`),
`validation.cpp:4981`(`EqualDestination`), `wallet.cpp` `SignReward`.

결과적으로 **합의는 허용하는데 지갑이 만들 수 없는 조합**(P2PK, P2WSH)과 **일부 경로만
통과하는 조합**이 공존한다. 이 상태로 Taproot 스테이킹을 얹으면 어느 함수에 어떤
분기를 추가해야 하는지 판단할 근거가 없다.

**할 일** 하드포크 페이로드 설계 전에 "스테이킹 가능 타입"을 한 곳에서 정의하고 네
함수가 모두 그것을 참조하게 한다. 이때 기존 체인에서 실제로 사용된 타입만 허용하도록
좁히는 것은 소프트포크, 넓히는 것은 하드포크임에 주의한다.

### B-4. 죽은 활성화 파라미터가 RPC에서 거짓을 보고한다

> **상태: 완료.** `consensus/params.h`, `versionbits.cpp`, `chainparams.cpp`에서 더미 `DEPLOYMENT_TAPROOT`를
> 완전히 제거했다. Taproot 활성화 기준은 `TaprootHeight` 단일 출처로 유지되며, `getblockchaininfo`의 거짓 보고가 해결되었다.

`DEPLOYMENT_TAPROOT`는 enum에 선언되고 세 네트워크 모두에 설정되어 `ALWAYS_ACTIVE`였으나 어디서도 읽히지 않고
실제 게이트는 `TaprootHeight`였다. 이로 인해 RPC에서 거짓 보고가 발생하던 문제를 파라미터 제거를 통해 해소했다.

### B-5. `BLOCK_SIGNATURE_ADDITION`의 실제 활성 상태를 확인해야 한다

블록 서명 강제는 BIP9 배포에 걸려 있다:

```2139:2141:src/validation.cpp
            if (VersionBitsState(pindex->pprev, chainparams.GetConsensus(), Consensus::BLOCK_SIGNATURE_ADDITION, versionbitscache) == ThresholdState::ACTIVE) {
                if (!CheckBlockSignature(block, state, chainparams.GetConsensus())) {
```

그런데 이 배포의 `nTimeout`은 `1585699200`(2020-04-01)로 이미 만료됐다
(`chainparams.cpp:112`). BIP9는 시한 내에 락인되지 못하면 `FAILED`로 영구 고정된다.
**`FAILED`라면 메인넷에서 블록 서명과 `nNonce == 0` 검사가 전혀 시행되지 않는 상태다.**

익스플로러의 최근 블록이 `Nonce = 0`인 것은 활성화됐다는 정황이지만 증거는 아니다.

**할 일** 메인넷 노드에서 `getblockchaininfo`의 `bip9_softforks` 항목으로 실제 상태를
확인한다. `FAILED`면 블록 서명 강제 자체가 하드포크 페이로드에 포함돼야 한다.

---

## C. 하드포크 전 필수 — 검증 인프라

### C-1. 활성화 메커니즘을 결정하고 테스트한다

> **상태: 핵심 구현 완료.** `TaprootHeight`와 별도의 `ColdStakingHeight`를 두되 메인넷은
> 둘 다 4,200,000, testnet/regtest는 0으로 설정했다. 단위 테스트가 각 높이 경계를,
> regtest 기능 테스트가 실제 블록 생성·전파·재색인을 검증한다. 구 버전 노드와의 명시적
> 상호운용 거부 시험은 릴리스 후보 단계에 남아 있다.

BIP9(versionbits)는 소프트포크용이다. 규칙을 **완화**하는 하드포크(Taproot 스테이킹
허용, 콜드 스테이킹 스크립트 도입)는 구 노드가 새 블록을 거부하므로, 조정된 높이 기반
활성화가 맞다. 이 코드베이스는 이미 `nSwitchHeight` / `TaprootHeight`로 높이 기반
게이팅을 하고 있어 형식이 일관된다.

**할 일**

1. `nHardForkHeight`(가칭)를 `Consensus::Params`에 추가하고 세 네트워크에 설정한다.
   메인넷 값은 배포 후 충분한 유예를 두고 정한다. §A-1에서 정하는 `TaprootHeight`와
   **같은 높이로 두는 것을 우선 검토한다** — Taproot가 아직 네트워크에서 활성이
   아니므로 Taproot 활성화, Taproot 스테이킹, 콜드 스테이킹을 한 번에 켤 수 있고,
   그러면 사용자가 겪는 업그레이드 이벤트가 한 번으로 줄어든다.
2. regtest에서 **활성화 전 / 경계 / 활성화 후** 세 구간을 각각 검증하는 functional
   테스트를 만든다. `feature_pos_staking.py`가 `nSwitchHeight` 경계에 대해 하는 것과
   같은 형태다.
3. 구 노드가 새 블록을 거부하는 것을 실제로 확인한다(하드포크임을 문서화하기 위해).

### C-2. 재구성과 `VerifyDB` 레벨 4 테스트가 없다

현재 functional 테스트는 재구성(reorg)과 `VerifyDB` 레벨 4를 다루지 않는다. 그런데
§B-2의 버그는 정확히 그 두 경로에서만 드러난다.

**할 일**

1. PoS 구간에서 재구성을 강제하는 functional 테스트를 추가한다(경쟁 체인 2개 →
   재구성 → 재검증).
2. `-checklevel=4 -checkblocks=0`으로 전체 재검증하는 테스트를 추가한다.
3. §B-2를 고치기 **전에** 이 테스트를 먼저 넣어 현재 동작을 고정한다.

### C-3. 체인 메타데이터가 2019년에 멈춰 있다

> **상태: 완료 (3,850,000 블록 기준 갱신).** `defaultAssumeValid`를 3,850,000 블록 해시로 설정하고,
> 마일스톤 체크포인트(1M, 2M, 3M, 3.85M)를 등록하였으며, `chainTxData`를 실제 누적 트랜잭션 11,735,025건 기준으로 최신화했다.

| 항목 | 갱신 전 | 갱신 후 (3,850,000 블록) |
|------|---------|-------------------------|
| 마지막 체크포인트 | 173,800 | **3,850,000** (1M, 2M, 3M, 3.85M 추가) |
| `nMinimumChainWork` | `0x00` | `0x00` (PoS 체인 특성상 유지) |
| `defaultAssumeValid` | `0x00` | **`b9435466a67b74f8f8fbaadbf42b5fafffce9deb17acd77f9c13f6e2d7c12769`** (IBD 가속) |
| `chainTxData` | 2019-04-16 (45만 건) | **2026-08 (1,173만 건, 0.0515 tx/s)** |

### C-4. 과거 체인 재검증 파이프라인이 없다

§B-1의 정수화와 §B-2의 플래그 수정은 둘 다 "과거 3,889,312블록에서 판정이 바뀌지
않는다"를 증명해야 안전하게 넣을 수 있다. 지금은 그걸 확인할 수단이 없다.

**할 일**

1. 메인넷 전체를 `-reindex-chainstate`로 재검증하는 절차를 문서화하고 CI 외부에서
   주기적으로 돌린다.
2. 정수 구현과 double 구현을 **동시에** 계산해 불일치 시 로그만 남기는 임시 계측
   빌드를 만들어, 실제 체인 데이터에서 차이가 나는 블록이 있는지 확인한다.

---

## D. 하드포크 페이로드 설계 선행 정리

여기까지는 "안전"이고, 이 구간은 "페이로드를 설계할 수 있는 상태 만들기"다.

### D-1. `pos/`가 노드 전역 상태를 되짚는 구조를 끊는다

`pos/kernel`과 `pos/stake`는 여전히 `validation.h`에서 `GetTransaction`,
`ReadBlockFromDisk`, `mapBlockIndex`를 가져온다. 그래서 순환 의존성
`pos -> validation -> pos` 두 개가 남아 있다
(`test/lint/lint-circular-dependencies.sh`에 근거와 함께 기록됨).

부수 효과가 크다. PoS 검증이 `txindex` 전체와 임의 디스크 읽기에 의존하므로 **프루닝이
불가능하고 IBD가 느리다**. 하드포크로 새 스크립트 타입을 추가하면 검증 경로가 더
무거워진다.

**할 일** 체인 접근을 인자로 주입하는 인터페이스를 도입하고, 가능한 범위에서 UTXO
기반 검증으로 옮긴다. `CheckProofOfStakePure`가 이미 그 방향의 부분 사례다.

### D-2. 합의 오케스트레이션을 `pos/`로 모은다

`VerifyCoinBaseTx`(약 85줄)와 `ConnectBlock`의 PoS 분기(약 60줄)가 아직
`validation.cpp`에 있다. 분할 보상 코인베이스의 규칙이 노드 코드에 흩어져 있으면
콜드 스테이킹 설계 시 보상 경로를 한 번에 검토할 수 없다.

### D-3. 콜드 스테이킹은 보상 리다이렉트 문제를 먼저 해결해야 한다

> **상태: 해결됨.** 활성화 후 콜드 스테이킹의 모든 코인베이스 보상 출력은 동일한
> P2WSH 콜드 계약으로만 지급되며, 블록 생성기도 로컬 보상 분배 설정을 무시하고 해당
> 계약에 100% 지급한다.
>
> **테스트넷 범위:** 운영자 수익화는 v1에서 의도적으로 제외한다. 주석 처리된 미완성
> 합의 코드를 남기지 않고, 테스트넷 검증 후 운영자 보상키·수수료율·상한을 포함한 별도
> v2 스크립트로 검토한다.

분할 보상 경로에는 **보상 수령 주소에 대한 제약이 없다.** `VerifyCoinBaseTx`는
코인스테이크 출력의 키가 보상 목록에 서명했는지만 확인한다:

```5079:5089:src/validation.cpp
    if (!EqualDestination(block.vtx[1], pubkey)) {
        return state.DoS(100, error("%s: pubkey does not match coinstake's output", __func__), REJECT_INVALID,
                         "bad-cb");
    }

    uint256 hash = pos::GetRewardHash(rewardValues, block.vtx[1], block.nTime);
    //printf("verify hash = %s\n",hash.ToString().c_str());
    if (pubkey.Verify(hash, vchSig)) {
        return true;
    }
    return state.DoS(100, error("%s: verification failed", __func__), REJECT_INVALID, "bad-cb");
```

즉 **코인스테이크 서명 키를 가진 쪽이 보상 수령지를 자유롭게 정한다.** 총액 상한만
있고(`validation.cpp:2149`) 하한도 없다(하한 검사는 `vout.size() <= 2`인 단순 경로에만
있다).

콜드 스테이킹은 서명 키를 핫 지갑에 두는 구조다. 위 규칙을 그대로 두면 **핫 노드가
원금은 못 건드리지만 보상 전액을 자기 주소로 돌릴 수 있다.** 이는 콜드 스테이킹의
전제를 무너뜨린다.

**할 일** 콜드 스테이킹 스크립트를 설계할 때 보상 수령지를 콜드 키에 고정하는 규칙을
합의 수준에서 함께 넣는다. 이 규칙 자체가 하드포크 항목이므로, 페이로드 설계 시점에
확정돼 있어야 한다.

### D-4. Taproot 스테이킹은 네 곳을 고쳐야 한다

> **상태: 구현 및 regtest 검증 완료.** P2TR 키 조회, 코인스테이크 공개키 추출,
> 목적지 비교, 블록·보상 Schnorr 서명을 `TaprootHeight`에 맞춰 연결했다.

Taproot 출력이 스테이킹되지 못하는 이유는 한 곳이 아니다. Taproot 자체가 아직
네트워크에서 활성이 아니므로(§0), 아래 세 개의 합의 변경은 §A-1이 정하는 활성화
높이에 함께 실을 수 있다. 그러면 "Taproot 켜기"와 "Taproot 스테이킹 허용"이 별개의
포크 이벤트가 되지 않는다.

| 위치 | 현재 | 필요한 변경 |
|------|------|-------------|
| `CWallet::SignReward` (`wallet.cpp`) | `GetKeyForDestination`이 P2TR에서 빈 `CKeyID` 반환 → `pubkey hash not found` | Taproot 키 조회 경로 (지갑, 합의 아님) |
| `pos::GetPubKeyFromScript` (`pos/stake.cpp:126`) | `default: return false` | `TX_WITNESS_V1_TAPROOT` 분기 (**합의**) |
| `EqualDestination` (`validation.cpp:4981`) | P2TR 분기 없음 | Taproot 목적지 비교 (**합의**) |
| `pos::CheckBlockSignature` (`pos/stake.cpp:185`) | `CPubKey::Verify` = ECDSA 전용 | x-only 키에 대한 Schnorr 검증 (**합의**) |

세 곳이 합의 변경이다. 특히 마지막 항목은 블록 서명 알고리즘을 키 타입에 따라
분기시키는 것이므로, `MakeBlockHashExcludedSignature`가 만드는 서명 대상 해시의
정의까지 함께 확정해야 한다.

---

## 정리: 의존 순서

```
[v0.27.0 정식 배포 전에 반드시]

A-1 + B-2  [완료]  (TaprootHeight → 4,200,000, bech32m 기본값 해제, 팁 의존 플래그 수정)
   └─ 함께 고쳤다. A-1만 하면 B-2가 발현하는 창이 열린다.
   └─ 남음: 활성화 높이 아래 witness v1 출력 전수 확인 (동기화된 노드 필요).
      contrib/devtools/scan-witness-programs.py

C-2  (재구성 / VerifyDB 레벨 4 테스트)
   └─ 원래는 B-2보다 먼저 넣어 현재 동작을 고정하는 것이 정석이었다. 배포 전에
      A-1을 넣어야 해서 순서를 바꿨으므로, 이제 가장 먼저 할 일이다.

B-5  (BLOCK_SIGNATURE_ADDITION 실제 상태 확인)
   └─ 페이로드 범위를 결정하므로 가장 먼저 확인. 동기화된 노드 필요.

B-4  (죽은 DEPLOYMENT_TAPROOT 제거)
   └─ C-1의 활성화 방식 결정에 선행.

A-2  (스테이킹 불가 / 미보호 잔액 가시화)
   └─ A-1 이후. 이미 만들어진 P2TR 잔액을 사용자가 회수할 수 있게 한다.

[하드포크 페이로드 설계 전에]

C-4  (전체 체인 재검증 파이프라인)
   └─ B-1의 안전성 증명 수단.

B-1  (부동소수점 → 정수)
   └─ 이번 하드포크에서 제외. 다중 플랫폼 동등성 증명과 C-4를 갖춘 별도 제안으로 유지.

B-3, D-1, D-2  (타입 집합 통일, 체인 접근 추상화, 오케스트레이션 이동)
   └─ 페이로드 설계의 전제.

C-1, C-3  [핵심 구현 완료]  (활성화 프레임, 체인 메타데이터)
   └─ 남음: 구 버전 노드 상호운용 거부 시험.

[하나의 조정된 활성화로 묶어서]

A-1의 TaprootHeight + D-4 + D-3  [구현 완료]
   └─ 메인넷 4,200,000에서 Taproot·Taproot 스테이킹·콜드 스테이킹을 함께 활성화한다.
```

**A-1은 코드 수정이 끝났다.** `TaprootHeight`가 과거로 설정된 채 v0.27.0이 배포되면
첫 P2TR 지출이 곧 체인 분기였고, 배포량이 0인 동안이라 파라미터와 기본값 수정으로
끝났다. 배포 후였다면 조정된 하드포크가 필요했다. 남은 것은 활성화 높이 아래
witness v1 출력이 없다는 전수 확인이다.

**B-1의 장기 위험은 남아 있지만 이번 페이로드에는 포함하지 않는다.** 기존 값과 완전한
동등성을 증명하기 전까지 배포 바이너리는 현재 보상 구현과 컴파일 조건을 유지해야 한다.
