# XPChain PoS 합의 규칙 명세 (P0-3)

이 문서는 [분리 로드맵](architecture-separation-roadmap.md)의 **P0-3(합의 파라미터 문서)** 산출물이다.
목적은 "PoS를 지키면서 비-PoS 코드를 이식"할 때, **무엇이 바뀌면 안 되는지**를 코드와 독립적으로
못 박아두는 것이다. P1 이후의 모든 리팩터는 이 문서에 적힌 값·수식·불변식을 바꾸지 않아야 한다.

기준 리비전: `src/validation.cpp`, `src/pos/`, `src/chainparams.cpp`
(XPChain Core 0.27.0, Bitcoin Core 0.17.0 기반).

> **주의:** 아래 값과 수식은 현재 메인넷을 검증하는 동작 그 자체다. 개선이 필요해 보이는 지점에는
> "관찰"로 표시해 두었으며, 이는 **커뮤니티 합의 없이 고칠 수 없다**(로드맵 §2-4).

---

## 1. PoW → PoS 전환

PoS는 별도 체인·별도 헤더가 아니라 **높이 기반 전환**이다.

```
IsPoSHeight(n) := n > nSwitchHeight        // src/validation.cpp
```

| 네트워크 | `nSwitchHeight` | 첫 PoS 높이 |
|---|---|---|
| main | 10275 | 10276 |
| test | 200 | 201 |
| regtest | 1680 | 1681 |

즉 `nSwitchHeight` 이하는 PoW 전용, 초과는 **PoS 전용**이다. 혼합 구간은 없다.

### 1.1 블록 헤더는 Bitcoin과 완전히 동일

`CBlockHeader`는 `nVersion / hashPrevBlock / hashMerkleRoot / nTime / nBits / nNonce` 80바이트
그대로이며 PoS 필드가 없다(`src/primitives/block.h`). `CTransaction`에도 `nTime`이 없다.

결과적으로 다음이 **모두 바닐라 Bitcoin과 호환**된다. 이식 시 건드릴 필요가 없다.

- 헤더 직렬화, `CDiskBlockIndex`, headers-first 동기화, compact block
- `nChainWork` 누적과 `FindMostWorkChain` 기반 체인 선택 (PoS용 `nChainTrust`/stake modifier가 없다)

PoS 고유 데이터는 **블록 헤더가 아니라 코인베이스 출력과 코인스테이크 트랜잭션**에 들어간다.

### 1.2 작업증명 검사의 우회

PoS 높이에서는 `CheckProofOfWork()`를 호출하지 않는다. 현재 이 분기는 아래 다섯 지점에 흩어져 있다.

| 위치 | 방식 |
|---|---|
| `CheckBlock()` | `CheckBlockHeader(..., fCheckPOW && !IsPoSHeight(nHeight))` |
| `AcceptBlockHeader()` 경로 | `CheckBlockHeader(..., !IsPoSHeight(pindexPrev->nHeight + 1))` |
| `ReadBlockFromDisk(pos, ...)` | `bool fProofOfStake` 인자로 PoW 검사 생략 |
| `ReadBlockFromDisk(pindex, ...)` | `IsPoSHeight(pindex->nHeight)`를 계산해 위로 전달 |
| `CBlockTreeDB::LoadBlockIndexGuts()` (`src/txdb.cpp:277`) | `nHeight <= nSwitchHeight`일 때만 검사 |

마지막 지점은 `IsPoSHeight()`를 쓰지 않고 `<= nSwitchHeight` 비교를 직접 손으로 적어놨다.
의미는 동일하지만(§1의 `>` 비교의 여집합) **비교 방향이 헷갈리기 쉬운 중복 표현**이므로,
P1에서 나머지 네 지점과 함께 단일 헬퍼로 모아야 한다.

**관찰 (P1-4에서 정리 대상):** `CheckBlock()`은 Bitcoin에서 문맥 독립(context-free) 함수인데,
XPChain은 그 안에서 `mapBlockIndex.find(block.hashPrevBlock)`로 높이를 역추적한다.
부모 헤더를 모르면 `nHeight = 0`으로 떨어져 PoS 블록에 PoW 검사가 적용된다.
headers-first 동기화에서는 부모 헤더가 항상 먼저 오므로 실동작에는 문제가 없지만,
`cs_main` 없이 전역 맵을 읽는 형태이며 최신 Bitcoin(`BlockManager` 분리)으로 이식할 때 그대로 옮길 수 없다.
P1에서 높이(또는 `pindexPrev`)를 **명시적 인자로 전달**하는 형태로 바꾼다. 규칙 자체는 불변이다.

---

## 2. PoS 블록의 구조 규칙

PoS 높이의 블록은 다음을 만족해야 한다.

1. `block.vtx.size() >= 2` — 코인베이스와 코인스테이크 (`CheckBlock`)
2. `block.vtx[1]`이 유효한 코인스테이크 (`IsCoinStakeTx`, `src/pos/stake_policy.cpp`)
   - `vin.size() == 1`
   - `vout.size() == 1`
   - `IsDestinationSame(prevTx->vout[n].scriptPubKey, vout[0].scriptPubKey)`
     — 스테이크한 UTXO와 **동일한 목적지로 되돌려 보내야** 한다
3. `BLOCK_SIGNATURE_ADDITION` 활성 시 (`ConnectBlock`)
   - `CheckBlockSignature(block)` 성공
   - `block.nNonce == 0`
4. 코인베이스 출력이 3개 이상이면 `VerifyCoinBaseTx(block)` 성공
5. 코인베이스 출력이 1~2개이면
   - `vout[0].nValue >= blockReward`
   - `IsDestinationSame(vtx[1]->vout[0].scriptPubKey, vtx[0]->vout[0].scriptPubKey)`
6. 항상: `vtx[0]->GetValueOut() <= blockReward`

`IsDestinationSame(a, b)`는 `Solver()` 결과가 각각 정확히 1개의 solution을 가지고
`ExtractDestination()`이 같은 `CTxDestination`을 낼 때만 참이다.

### 2.1 다중 수령자 코인베이스 (`VerifyCoinBaseTx`)

코인베이스 `vout[0].scriptPubKey`는 `OP_RETURN <출력개수> <서명> <pubkey>` 형태의 데이터 출력이다.

- `vout[0].nValue == 0`, `vout.back().nValue == 0` (witness commitment)
- `<출력개수> == vout.size() - 2`
- `EqualDestination(vtx[1], pubkey)` — pubkey가 코인스테이크 출력의 목적지와 일치
  (P2SH-P2WPKH / P2PKH / P2WPKH, 활성화 후 P2TR 및 콜드 스테이킹 지원)
- `GetRewardHash(rewardValues, vtx[1], block.nTime)`를 ECDSA로 검증하며,
  P2TR 코인스테이크는 64바이트 Schnorr 서명도 검증한다

여기서 `rewardValues`는 `vout[1..size]`의 `(scriptPubKey, nValue)` 목록이고,

```
GetRewardHash := Hash( Σ(scriptPubKey ‖ nValue) ‖ nTime ‖ vtx[1]->vin[0] )   // SER_GETHASH
```

이 형태는 예외가 아니라 **기본 경로**다. 마인터의 `GetRewardPct()`(`src/miner.cpp`)가
지갑의 `vRewardDistributionPcts`(스테이킹 보상 분배 설정)로 수령자 목록을 만들고,
남은 지분을 기본 목적지에 배정하므로 목록은 항상 최소 1개다. 따라서 분배를 설정하지 않은
지갑도 코인베이스 출력이 3개(`OP_RETURN` + 보상 1개 + witness commitment)가 되고
`VerifyCoinBaseTx()`를 통과해야 한다. §2의 5번(출력 1~2개 경로)은 다른 구현이 만든
블록을 위한 경로다.

### 2.2 블록 서명 (`CheckBlockSignature`)

블록 서명은 헤더가 아니라 **코인베이스 `scriptSig` 말미**에 실린다.
`MakeBlockHashExcludedSignature()`가 서명을 잘라낸 코인베이스로 머클루트를 재계산하여
"서명 제외 블록 해시"를 만들고, 코인스테이크 입력에서 뽑은 pubkey들 중 하나로 검증한다.

이 방식 덕분에 헤더 포맷이 바닐라로 유지된다. 이식 시 이 성질을 깨지 않아야 한다.

### 2.3 Taproot 스테이킹 (하드포크 활성화 후)

`TaprootHeight`부터 `TX_WITNESS_V1_TAPROOT` 코인스테이크가 허용된다. 출력의 32바이트
x-only 공개키를 블록 서명키로 추출하고, 블록 서명과 다중 수령자 보상 서명을 BIP340
Schnorr로 생성·검증한다. 활성화 전에는 이 공개키 추출이 실패하므로 과거 블록의 검증 규칙은
바뀌지 않는다. 지갑은 키의 짝·홀수 공개키 표현을 모두 확인하여 해당 x-only 출력키의
개인키를 찾는다.

단위 테스트는 활성화 직전 거부, 활성화 높이 수락, 변조된 Schnorr 서명 거부를 고정한다.
기능 테스트는 실제 bech32m UTXO로 PoS 블록을 생성하고 다른 노드의 수용까지 확인한다.

### 2.4 콜드 스테이킹 (하드포크 활성화 후)

`ColdStakingHeight`부터 P2WSH 내부에 다음 두 경로를 가진 계약을 사용할 수 있다.

- 스테이킹 경로: 스테이킹키 서명과 `OP_CHECKCOLDSTAKEVERIFY`를 요구한다. 입력 1개와 출력
  1개만 허용하고, 출력 스크립트와 금액이 원래 계약·원금과 정확히 같아야 한다.
- 출금 경로: 출금키 서명으로 일반 지갑 거래를 만들 수 있으며 위 covenant를 적용하지 않는다.

따라서 위임 노드는 보상을 생성할 수 있지만 원금을 다른 주소로 보내거나 일부를 수수료로
사용할 수 없다. 콜드 코인스테이크 자체의 수수료는 0이며 PoS 보상은 기존과 동일하게
코인베이스에 생성되되, 모든 보상 출력은 합의 규칙상 동일 콜드 계약으로 돌아가야 한다.
로컬 보상 분배 설정으로 위임 노드가 보상을 가로챌 수 없다. 계약 생성 RPC는
`createcoldstakingaddress "owner_address" "staker_address"`이며, 출금 지갑과 스테이킹
노드가 동일한 witness script를 저장해야 하므로 양쪽에서 실행하는 것이 운영 원칙이다.

`OP_CHECKCOLDSTAKEVERIFY`는 기존 `OP_NOP10`을 재사용한다. 활성화 전에는 과거와 동일한 NOP,
활성화 높이부터는 covenant 검사로 동작하므로 과거 체인 재검증 결과를 바꾸지 않는다.

현재 테스트넷 검증 대상은 **콜드 스테이킹 v1(운영자 수수료 없음)**이다. 운영자 보상률과
보상 주소를 포함하는 모델은 v1 코드에 비활성 분기로 넣지 않으며, 테스트넷 결과를 확인한
뒤 별도의 버전 스크립트와 합의 규칙으로 설계한다. v1에서는 로컬 보상 분배 설정과 관계없이
보상 100%가 원래 콜드 계약으로 귀속된다.

---

## 3. 커널 해시 (`CheckStakeKernelHash`)

`src/pos/kernel.cpp`. PoS의 심장이며 **바이트 단위로 보존해야 하는 유일한 해시 규칙**이다.

### 3.1 나이 요건

```
nTimeBlockFrom + nStakeMinAge > nTimeTx  ⇒  실패
```

| 네트워크 | `nStakeMinAge` | `nStakeMaxAge` |
|---|---|---|
| main | 259 200 (3일) | 5 184 000 (60일) |
| test | 3 600 (1시간) | 5 184 000 (60일) |
| regtest | 10 (10초) | 8 640 000 (100일) |

### 3.2 해시 입력 (직렬화 순서)

`CDataStream(SER_GETHASH, 0)`에 아래 순서로 쓰고 `Hash()`(double-SHA256)를 취한다. 총 28바이트.

| # | 값 | 타입 | 바이트 |
|---|---|---|---|
| 1 | `nBits` | `unsigned int` | 4 (LE) |
| 2 | `nTimeBlockFrom` | `uint32_t` | 4 (LE) |
| 3 | `nTxPrevOffset` | `unsigned int` | 4 (LE) |
| 4 | `nTimeBlockFrom` | `uint32_t` | 4 (LE) |
| 5 | `n` (prevout index) | `uint64_t` | 8 (LE) |
| 6 | `nTimeTx` | `uint32_t` | 4 (LE) |

**`nTimeBlockFrom`이 2번과 4번에 두 번 들어간다.** Peercoin 계열 원본은 4번 자리가
"이전 트랜잭션의 `nTime`"이지만 XPChain은 트랜잭션에 `nTime`이 없으므로 블록 시각으로 대체했다.
중복은 의도된 것이며 **버그로 오인해 고치면 즉시 체인이 갈라진다.**

`nTxPrevOffset`은 이전 블록 안에서 트랜잭션 배열이 시작하는 오프셋이다.

```
nTxPrevOffset = GetSizeOfCompactSize(blockFrom.vtx.size()) + sizeof(CBlockHeader)   // = ... + 80
```

즉 이전 블록의 **트랜잭션 개수**에만 의존한다(개별 tx 위치가 아니다).

### 3.3 성공 조건

```
bnTargetPerCoinDay = arith_uint256().SetCompact(nBits)
nTimeWeight        = min(nTimeTx - nTimeBlockFrom, nStakeMaxAge) - nStakeMinAge
bnCoinDayWeight    = arith_uint256(nAmount) * nTimeWeight / COIN / 86400      // 정수 나눗셈

성공 ⟺ uint512(hashProofOfStake) <= uint512(bnCoinDayWeight) * uint512(bnTargetPerCoinDay)
```

- `nAmount`는 스테이크한 UTXO의 `nValue`(satoshi)다.
- 나눗셈이 정수이므로 `nAmount/COIN * nTimeWeight < 86400`이면 `bnCoinDayWeight == 0`이 되어
  **어떤 해시로도 성공할 수 없다.** (regtest 시나리오 설계 시 반드시 고려)
- 512비트 비교는 `arith_uint512`(XPChain 추가, `src/arith_uint256.h`)를 쓴다.

---

## 4. 코인스테이크 전체 검증 (`CheckProofOfStake`)

`ConnectBlock()` 진입 직후 호출된다(`src/validation.cpp`).

```
CheckProofOfStake(block.vtx[1], block.nBits, hashProofOfStake, block.nTime, pindex->nHeight, consensus)
```

수행 순서:

1. `GetTransaction(vin[0].prevout.hash, txPrev, ..., /*fAllowSlow=*/true)` — 이전 tx와 그 블록 해시
2. 모든 입력의 이전 출력을 모아 `PrecomputedTransactionData`를 구성
3. `CScriptCheck(txPrev->vout[n], tx, 0, nFlags, true, &txdata)` — 입력 0의 스크립트 검증
   - `nFlags = GetCoinStakeScriptFlags(nHeight, params)`: 검증 중인 블록의 높이가
     `TaprootHeight` 이상이면 `SCRIPT_VERIFY_TAPROOT`, `ColdStakingHeight` 이상이면
     `SCRIPT_VERIFY_COLDSTAKE` 추가
4. `mapBlockIndex`에서 이전 블록 인덱스를 찾고 `ReadBlockFromDisk()`로 **블록 전체를 읽음**
5. `CheckStakeKernelHash(nBits, prevBlock.GetBlockTime(), offset, txPrev->vout[n].nValue, n, block.nTime, ...)`

### 4.1 데이터 의존성 (이식 최대 난관)

4단계는 임의의 과거 블록을 디스크에서 읽고, 1단계는 `-txindex` 또는 UTXO 기반 느린 경로에 의존한다.
최신 Bitcoin은 **검증 경로가 txindex에 의존하지 않는다**는 것을 전제로 하며,
`mapBlockIndex`/`chainActive` 전역도 `BlockManager`/`ChainstateManager`로 대체되었다.

**관찰:** 커널 해시가 실제로 필요한 값은 다음뿐이다.

| 필요한 값 | 현재 출처 | 대체 가능한 출처 |
|---|---|---|
| `nAmount` | `txPrev->vout[n].nValue` (txindex) | `CCoinsViewCache`의 `Coin::out.nValue` |
| `scriptPubKey` (서명 검증용) | `txPrev->vout[n]` (txindex) | `Coin::out.scriptPubKey` |
| `nTimeBlockFrom` | `ReadBlockFromDisk` 후 헤더 | `pindex->nTime` |
| `nTxPrevOffset` | `ReadBlockFromDisk` 후 `vtx.size()` | `pindex->nTx` (`src/chain.h`) |
| 이전 블록 인덱스 | `mapBlockIndex[hashBlock]` | `Coin::nHeight` → 활성 체인 조회 |

`ConnectBlock()` 시점에는 코인스테이크의 입력이 아직 UTXO 집합에 살아 있으므로,
위 값 전부를 **디스크 읽기 없이, txindex 없이** 얻을 수 있다.
이 치환은 규칙을 바꾸지 않으면서 프루닝 호환성과 O(1) 검증을 동시에 얻는다.
P1(경계 고정) → P5(수렴) 사이에서 다룬다.

### 4.2 Taproot 플래그의 문맥 (해결됨)

3단계의 플래그 판정은 한때 `chainActive.Height() + 1 >= TaprootHeight`였다. 검증 중인
**블록의 높이**가 아니라 **현재 활성 팁의 높이**를 봤으므로, 재구성(reorg)이나 `VerifyDB`
레벨 4 재검증 경로에서 같은 블록이 상황에 따라 다른 플래그로 판정될 수 있었다. 지금은
`GetCoinStakeScriptFlags(nHeight, params)`가 검증 대상 블록의 높이만 본다
(`src/pos/kernel.cpp`). 단위 테스트 `coinstake_script_flags_follow_block_height`가 경계를
고정한다.

| 네트워크 | `TaprootHeight` | `ColdStakingHeight` |
|---|---:|---:|
| main | 4 200 000 | 4 200 000 |
| test | 0 | 0 |
| regtest | 0 | 0 |

메인넷 활성 높이는 미래에 있다. 활성화 경계를 지나는 동안 `chainActive.Height() + 1`은
동기화 진행에 따라 움직이고 `pindex->nHeight`는 고정이므로, 위 수정이 없으면 바로 그
구간에서 분기가 났다.

같은 함수의 2단계도, 일부 입력의 이전 tx를 못 찾으면 `spent_outputs`를 버리고
`PrecomputedTransactionData(*tx)`로 조용히 되돌아간다. Taproot 활성 후에는 sighash가 달라져
검증이 실패하게 되므로, §4.1의 UTXO 기반 치환으로 이 폴백 자체를 제거하는 것이 옳다.

---

## 5. PoS 보상 (`GetProofOfStakeReward`)

`src/validation.cpp`. `ConnectBlock()`이 계산하는 PoS 블록의 상한 보상이다.

```
blockReward = GetProofOfStakeReward(pindex->nHeight, nAmount, nAge, consensus)

nAmount = txPrev->vout[ vtx[1]->vin[0].prevout.n ].nValue     // 스테이크한 금액
nAge    = block.nTime - prevBlockHeader.nTime                 // uint32_t
```

**PoW 보상과 달리 수수료(`nFees`)를 더하지 않는다.** PoW 구간만
`blockReward = nFees + GetBlockSubsidy(...)`이고, `GetBlockSubsidy`는 `11000000 * COIN`을
`nHeight / nSubsidyHalvingInterval`만큼 우측 시프트한 값이다.

### 5.1 연 이율 (`GetAnnualRate`)

`nSubsidyReducingInterval = 60 * 24 * 365 = 525600` 블록.

| 높이 구간 | 연 이율 |
|---|---|
| PoS 이전 | 0 |
| ~ 525 600 | 0.10 |
| 525 601 ~ 1 051 200 | 0.09 |
| 1 051 201 ~ 1 576 800 | 0.08 |
| 1 576 801 ~ 2 102 400 | 0.07 |
| 2 102 401 ~ 2 628 000 | 0.06 |
| 2 628 001 ~ | 0.05 |

### 5.2 나이 계수와 최종 식

```
M = 1.025   (dRewardCurveMaximum)
B = 0.018   (dRewardCurveBase)
L = 1.0     (dRewardCurveLimit)
S = 0.00000285 (dRewardCurveSteepness)

if nAge < nStakeMinAge:  return 0
nAge = min(nAge, nStakeMaxAge)

coefficient = min( M / (1 + (M/B - 1) * exp(-S * nAge)), L )
reward      = (CAmount)( nAmount * GetAnnualRate(nHeight) * coefficient * nAge / 31536000 )
```

`31536000 = 365 * 24 * 60 * 60`. 마지막 `(CAmount)` 캐스팅은 **0 방향 절단**이다.

성질(P0-2 단위 테스트가 검증):

- `nAge < nStakeMinAge` → 0
- `nAge`에 대해 단조 증가 (`nStakeMaxAge`에서 포화)
- `coefficient`는 `nAge` 하나만의 함수이고, 로지스틱 곡선이 `L = 1.0`에 걸리는
  `nAge ≈ ln((M/B - 1) / (L/M - 1... ))` 지점 이후로는 정확히 1.0으로 고정된다
- PoS 이전 높이 → 0

### 5.3 부동소수점 합의 (이번 하드포크에서는 변경하지 않음)

`GetAnnualRate`는 `double_t`를 반환하고 보상 계산은 `exp()`와 `double` 산술을 쓴다.
**즉 합의 결과가 컴파일러·libm·최적화 플래그·타깃 아키텍처에 의존한다.**
현재 트리는 이미 C++17(`configure.ac`의 `AX_CXX_COMPILE_STDCXX([17])`)이지만,
최신 Bitcoin Core로 갈수록 요구 툴체인이 올라가므로 장기적으로 별도 검토가 필요하다.

다행히 위험은 유한하고 검증 가능하다.

- `coefficient`는 **정수 하나(`nAge`)의 함수**다. 정의역은 `[nStakeMinAge, nStakeMaxAge]`,
  메인넷 기준 4 924 801개 값뿐이다 → **전수 차분 테스트로 대체 구현의 동등성을 완전히 증명할 수 있다.**
- `GetAnnualRate`가 반환하는 여섯 값(0.10~0.05)은 유리수로 정확히 표현된다.
- 최종 절단이 정수를 만들므로, 목표는 "같은 정수를 내는 결정적 구현"이다.

기존 메인넷 결과와 1사토시까지 완전히 같다는 증명 없이 수식을 바꾸는 위험이 더 크므로,
이번 콜드 스테이킹·Taproot 하드포크에서는 **보상 수식과 연산 순서를 전혀 변경하지 않는다.**
정수화는 운영체제별 골든 벡터와 전체 정의역 차분 결과를 확보한 뒤 별도 제안으로 분리한다.

### 5.4 언더플로 불변식 (리팩터 시 깨지기 쉬움)

`nAge`는 `uint32_t`이고 `block.nTime - prevHeader.nTime`으로 계산된다.
`block.nTime < prevHeader.nTime`이면 **언더플로로 거대한 값이 되고, 곧바로 `min(nAge, nStakeMaxAge)`가
이를 `nStakeMaxAge`로 잘라내어 최대 보상을 주게 된다.**

현재 이것이 악용 불가능한 이유는 오직 다음 순서 때문이다.

1. `ConnectBlock()`은 **함수 진입 직후** `CheckProofOfStake()`를 먼저 호출한다.
2. `CheckStakeKernelHash()`가 `nTimeBlockFrom + nStakeMinAge > nTimeTx`를 거부한다.
3. 따라서 이후 보상 계산 지점에서는 `block.nTime >= prevHeader.nTime + nStakeMinAge`가 보장된다.

**이 호출 순서는 합의 불변식이다.** `ConnectBlock`을 재배치하거나 PoS 훅을 분리할 때
(P1-4, P5-1) 순서가 바뀌면 인플레이션 취약점이 열린다. P1의 회귀 테스트가 반드시 덮어야 한다.

---

## 6. 난이도 (`pow.cpp`)

**PoS는 별도의 난이도 조정 알고리즘을 가진다.** 함수 이름과 `nBits` 필드를 PoW와 공유하기
때문에 눈에 잘 안 띄지만, `GetNextWorkRequired()`와 `CalculateNextWorkRequired()` 양쪽에
`pindexLast->nHeight > nSwitchHeight` 분기가 들어 있다.

| 항목 | PoW | PoS |
|---|---|---|
| 조정 주기 | `DifficultyAdjustmentInterval()`(2016)의 배수 높이에서만 | **매 블록** (주기 스킵 분기가 PoW 전용) |
| 관측 창 | 직전 2015블록 (`nHeight - (interval - 1)`) | **직전 1블록** (`nHeight - 1`) |
| `nActualTimespan` 클램프 | `[timespan/4, timespan*4]` | **없음** |
| 최소난이도 예외 (`fPowAllowMinDifficultyBlocks`) | 적용 | 미적용 |
| 재타깃 식 | `bnNew *= actual; bnNew /= nPowTargetTimespan` | 아래 |

```
nInterval = nPowTargetTimespan / nPowTargetSpacing
bnNew *= ((nInterval - 1) * nPowTargetSpacing + nActualTimespan + nActualTimespan);
bnNew /= ((nInterval + 1) * nPowTargetSpacing);
```

즉 매 블록 이전 블록과의 간격만 보고 목표 간격(`nPowTargetSpacing`, 60초)으로 끌어당기는
1차 필터다. `nActualTimespan`이 0에 가까우면 배율이 `(nInterval-1)/(nInterval+1)`로 수렴하므로
블록당 최대 감쇠폭이 유한하다. 상한은 PoW와 동일한 `powLimit`이다.

`fPowNoRetargeting`(regtest)은 `CalculateNextWorkRequired()` 진입 즉시 `pindexLast->nBits`를
반환하므로 PoS 경로에도 그대로 적용된다.

PoS 전용 `posLimit`은 없고, 계산된 `nBits`가 §3.3의 커널 해시 목표로 재사용된다.

**관찰:** 스테이킹 측 `GetnBits()`(`src/miner.cpp`)는 `GetNextWorkRequired()`를 거치지 않고
`CalculateNextWorkRequired(pindexLast, pindexLast->pprev->GetBlockTime(), params)`를 직접
호출한다. PoS 분기의 관측 창이 1블록이므로 결과는 같지만, **두 곳에 같은 규칙이 중복 표현되어
있다.** 로드맵 P5-2(`pos/difficulty.cpp`)에서 하나로 합쳐야 한다.

---

## 7. XPChain 고유 버전비트 배포

`src/consensus/params.h`의 `DeploymentPos`에 Bitcoin에 없는 항목이 있다.

| 배포 | bit | main / test | regtest |
|---|---|---|---|
| `DEPLOYMENT_CHECK_DUP_TXIN` | 3 | — | ALWAYS_ACTIVE |
| `BLOCK_SIGNATURE_ADDITION` | 2 | 2019-04-01 ~ 2020-04-01 | ALWAYS_ACTIVE |
| `DEPLOYMENT_TAPROOT` | — | 높이 기반 (`TaprootHeight`) | 0 |
| 콜드 스테이킹 | — | 높이 기반 (`ColdStakingHeight`) | 0 |

`DEPLOYMENT_TAPROOT`는 세 네트워크 모두에 설정되어 있지만 **어디서도 읽히지 않는다.** 실제
게이트는 `TaprootHeight`다. `getblockchaininfo`가 이 배포를 근거로 거짓을 보고하는 문제는
`doc/pre-hardfork-checklist.md` §B-4에 있다.

`BLOCK_SIGNATURE_ADDITION`은 §2.2의 규칙을 활성화한다. 다만 `nTimeout`이 2020-04-01로
만료돼 있어 메인넷에서 실제로 `ACTIVE`인지 `FAILED`인지는 확인이 필요하다(체크리스트 §B-5).

---

## 8. 이식 시 절대 바뀌면 안 되는 목록 (체크리스트)

| # | 항목 | 근거 |
|---|---|---|
| 1 | `nSwitchHeight` 및 `IsPoSHeight`의 **초과(`>`)** 비교 | §1 |
| 2 | 커널 해시의 6개 필드 순서·타입, `nTimeBlockFrom` 중복 | §3.2 |
| 3 | `nTxPrevOffset` 계산식 (compact size + 80) | §3.2 |
| 4 | `nTimeWeight` / `bnCoinDayWeight`의 **정수 나눗셈** 순서 | §3.3 |
| 5 | 512비트 비교(256비트로 줄이면 오버플로 거동이 달라짐) | §3.3 |
| 6 | 코인스테이크 구조 규칙 (`vin==1`, `vout==1`, 동일 목적지) | §2 |
| 7 | `GetRewardHash` 직렬화, 블록 서명 제외 해시 계산 | §2.1, §2.2 |
| 8 | 보상 공식의 상수·연산 순서·최종 절단 | §5.2 |
| 9 | PoS 보상에 **수수료를 더하지 않음** | §5 |
| 10 | `CheckProofOfStake` → 보상 계산의 **호출 순서** | §5.4 |
| 11 | PoS 재타깃: 매 블록 / 1블록 창 / 클램프 없음 / 전용 식 | §6 |
| 12 | PoW 검사를 건너뛰는 다섯 지점 (`txdb.cpp` 포함) | §1.2 |

---

## 9. 관련 문서

- [분리 방식 절차 (P0~P6 로드맵)](architecture-separation-roadmap.md)
- 단위 테스트: `src/test/pos_tests.cpp` (P0-2)
- 기능 테스트: `test/functional/feature_pos_staking.py` (P0-4)
