# 実出力ランタイムテスト計画

この文書は、OBS Studio 32.2.2向けYouTube RTMPS出力アダプターの出荷可否を判断するためのテスト計画です。現在のリポジトリには実出力アダプター、ランタイム状態機械、Frontend bridgeが含まれています。値レベルのランタイムテストは自動化済みですが、実OBS outputのfault injection、メモリ検査、実サービスへの限定公開配信は引き続き出荷ゲートです。

対象はWindows x64、OBS Studio 32.2.2、OBSのnative Twitch出力を維持したまま、追加のYouTube RTMPS出力を1つ開始・停止する構成です。YouTube側の失敗はTwitch側へ波及してはいけません。

## 1. 判定基準

### 出荷を止める項目

次のいずれかが1件でも再現した場合は出荷不可です。

- クラッシュ、use-after-free、double release、解放済みOBSオブジェクトへのアクセス。
- YouTubeの失敗・切断・資格情報エラーによって、native Twitch出力が停止・再起動される。
- 出力・サービス・エンコーダー・コールバックコンテキストの所有権が不明確なまま、次の出力開始と重複する。
- `EXIT`、プロファイル変更、OBS終了後にプラグインコードへコールバックされる、またはOBS終了がハングする。
- YouTubeのStream key、RTMPS URLに含まれる秘密情報、認証情報がログ・ダンプ対象の診断情報・プロファイルへ出力される。
- 古い世代・古い試行のコールバックが、新しいセッションの状態を変更する。

### 合格の最低条件

- 自動テスト、ランタイムのfault-injectionテスト、手動OBSテストのP0/P1項目がすべて合格する。
- `obs_output_start()` の戻り値、`start`シグナル、出力エラー、`stop`シグナルを混同しない。
- 出力を止めた後、完全なrelease完了が通知されるまで新しいYouTube出力を作らない。
- 連続したStart/Stop、プロファイル切り替え、終了処理を繰り返しても、出力の同時存在・ハンドル・スレッド・メモリ使用量が増え続けない。
- 手動の実配信は、テスト用のTwitchアカウントとYouTubeの非公開または限定公開イベントで行い、実ユーザーの配信先・キーを使用しない。

P0は安全性・所有権・秘密情報・native出力保護に関わる項目、P1は正常な開始停止・失敗表示・世代整合性に関わる項目です。P2は診断や表示の改善で、未解決なら出荷判断時に明示的なwaiverを残します。

## 2. テスト環境と隔離

- OBS Studioは必ず32.2.2の同一ビルドを使用し、テスト対象のコミット、OBSビルド、Windowsビルド、GPUドライバーを記録する。
- OBSはリポジトリ内の一時的なportable環境、または専用のテストプロファイルで起動する。普段使いのOBSプロファイル、シーンコレクション、資格情報をテストで上書きしない。
- YouTubeはテスト用チャンネルの限定公開または非公開ストリーム、Twitchはテスト用アカウントを使用する。初回の資格確認やライブ有効化に必要な時間はテスト結果と分けて記録する。
- 実際のStream keyは、スクリプト、ソース、テスト結果、ログ、画面キャプチャへ保存しない。テストケースの入力値は `<redacted>` または無効な固定文字列を使い、実配信テストの証跡はキーの有無と末尾数文字だけにする。
- Credential Managerの通常APIテストは注入したfake実装で行う。OSの資格情報へアクセスする手動テストを行う場合は専用のテストアカウント・テストtargetを使い、終了後にプラグインのRemove操作で削除する。
- ネットワーク切断は、可能ならfake outputまたは専用のテストプロキシで再現する。OS全体のファイアウォールや普段のネットワーク設定を恒久的に変更しない。手動の切断試験を行った場合は、試験後に設定が元に戻ったことを確認する。
- ログ・トレースはリポジトリ外の一時成果物へ保存し、提出前にStream key、OAuth token、認証コード、URLとkeyを連結した文字列をマスクする。

## 3. 実行順序

1. リポジトリローカルの依存関係を使ってRelWithDebInfoをビルドする。
2. `ctest --test-dir <build> --output-on-failure` を実行し、既存のunit/session/UI/localeテストを通す。
3. OBS APIをfake化したランタイムテストを、通常・エラー・再入・遅延イベントの順で実行する。
4. ASan/UBSanが利用できるビルドではすべてのランタイムテストを実行する。Windowsで同一条件が難しい場合は、専用の隔離環境でApplication Verifier/PageHeapまたはOBSのデバッグビルドを使い、代替手段を記録する。
5. 手動OBSテストを、まず接続なしの検証、次に限定公開の実配信、最後に終了・プロファイル変更・繰り返し試験の順に行う。
6. すべてのP0/P1結果、ログ、テスト環境、未実施項目、waiverを1つのローカル成果物にまとめる。秘密情報は含めない。

## 4. 自動ランタイムハーネスの要件

実装時には、OBS本体・YouTube・Credential Managerの結果を直接待つだけでなく、次のfault-injection境界をテストから制御できるようにします。fake境界のAPI形状は実装方式に合わせてよいですが、productionコードにテスト用の秘密ログや強制終了を持ち込まないことが条件です。

- Frontendイベントを任意の順序・遅延で投入できる。対象は `STREAMING_STARTED`、`STREAMING_STOPPING`、`STREAMING_STOPPED`、`PROFILE_CHANGING`、`PROFILE_CHANGED`、`EXIT`。
- native出力の開始受理、同期拒否、`starting`シグナル、遅延した開始通知を個別に再現できる。
- YouTube出力の `obs_output_start()` 戻り値、`starting`、`start`、`reconnect`、`reconnect_success`、`stopping`、`stop`、`deactivate`、エラーを個別に制御できる。
- OBSの出力・サービス・音声エンコーダー・映像エンコーダー・コールバックコンテキストについて、retain/release、signal connect/disconnect、stop要求の回数と順序を秘密情報なしで記録できる。
- Credential Managerのread成功、未登録、アクセス拒否、OSエラー、サイズ不正、型不正を返せる。
- queued callbackを任意のタイミングで実行でき、プロファイル変更、新しい試行、dock/controller破棄の前後に古いイベントを配送できる。
- 呼び出し元スレッドを明示できる。OBS callbackからSessionCoordinatorを直接呼ばず、所有スレッドへ値イベントをpostしていることを検証する。

## 5. ランタイム自動テスト行列

### 5.1 開始・接続・停止

| ID | 優先度 | 前提と操作 | 合格条件 | 区分 |
|---|---|---|---|---|
| RT-START-01 | P0 | recognized Twitch、YouTube有効、keyあり。native開始が受理され、`STREAMING_STARTED`後にYouTube開始effectを処理する | YouTube output/serviceを1組だけ作成し、`obs_output_start()`の戻り値がtrueでも状態は`Connecting`のまま。nativeへStart/Stopを呼ばない | 自動 |
| RT-START-02 | P1 | RT-START-01後にYouTube outputの`start`シグナルを1回配送 | YouTubeだけが`Streaming`になり、開始通知が1回だけ進む。遅延中に`Streaming`を表示しない | 自動＋手動 |
| RT-START-03 | P1 | 同じYouTube leaseに重複した`start`、`reconnect_success`、遅延イベントを配送 | 状態・revision・effectが重複して進まない。二重outputを作らない | 自動 |
| RT-START-04 | P0 | `obs_output_start()`がfalseを返す | `youtubeReleased(lease)`相当の完全releaseを1回だけ行い、YouTubeは`Failed`。native Twitchは`Streaming`のまま | 自動＋手動 |
| RT-START-05 | P0 | startはtrueだが`start`シグナルなし。遅延後に接続エラーまたはreleaseを配送 | start受理を失敗と誤判定しない。タイムアウトで別outputを重ねず、terminal release後だけ`Failed`に遷移する | 自動 |
| RT-START-06 | P1 | YouTubeが接続前に`stopping`/`stop`/`deactivate`を返す | start途中のoutputも完全releaseされ、nativeは継続。古いstart callbackが再接続を開始しない | 自動 |
| RT-STOP-01 | P0 | native `STREAMING_STOPPING`でYouTubeが`Streaming` | YouTubeへStop要求を1回だけ発行し、native停止を要求しない。出力signal callback内で最終releaseしない | 自動＋手動 |
| RT-STOP-02 | P0 | native `STREAMING_STOPPED`がYouTube releaseより先に到着 | `youtubeReleased`までteardown barrierを閉じたまま保持し、新しいYouTube outputを作らない | 自動 |
| RT-STOP-03 | P1 | native Stop、YouTube stop、deactivate、releaseを重複・逆順で配送 | Stop effect、signal disconnect、releaseが各1回。revisionが不要に進まず、クラッシュしない | 自動 |
| RT-STOP-04 | P1 | YouTubeだけが予期せず切断・停止する | YouTubeは`Failed`または契約で定めた停止状態になり、Twitch nativeは`Streaming`を維持。即時の無限再起動をしない | 自動＋手動 |

### 5.2 native開始拒否・高速再起動・世代

| ID | 優先度 | 前提と操作 | 合格条件 | 区分 |
|---|---|---|---|---|
| RT-NATIVE-01 | P0 | OBSのnative `StartStreaming()`を同期拒否させる。OBS 32.2.2の仕様どおり`STREAMING_STOPPED`が来ない経路を使う | Starting signalが観測されなかった同じnative leaseだけを、Frontend callback後の1回のqueued reconciliationで`nativeStartFailed`にする。YouTube outputは作らない | 自動＋手動 |
| RT-NATIVE-02 | P0 | native開始を受理したが、映像接続が遅れている状態でreconciliationを走らせる | 遅い接続を同期拒否と誤判定しない。`starting`観測済みのleaseは維持する | 自動 |
| RT-NATIVE-03 | P1 | native同期拒否後にもう一度Startする | 新しいNativeLeaseを払い出し、前のleaseのlate eventが新しい試行を変更しない | 自動＋手動 |
| RT-RACE-01 | P0 | Twitch/YouTube配信中にnative Stopし、YouTube完全release前にnative Startする | 旧outputが完全releaseされるまで新YouTube outputを作らない。旧・新outputの同時data capture、encoder callback、stop要求がない | 自動＋手動 |
| RT-RACE-02 | P0 | RT-RACE-01の旧native `STOPPED`、旧YouTube `start`/`stop`/`reconnect`を新試行後に配送 | 旧NativeLease/OutputLeaseはno-op。新しいsnapshot、revision、effectを変更しない | 自動 |
| RT-GEN-01 | P0 | YouTube Connecting/Streaming/Stopping中に`PROFILE_CHANGING`→`PROFILE_CHANGED`。旧outputのcallbackを新profileの読み込み前後に配送 | generationを先にinvalidateし、旧outputをretireする。新profileの設定・key・service pointerを旧callbackが上書きしない | 自動＋手動 |
| RT-GEN-02 | P1 | 同一profileで複数回のqueued status callbackを逆順に配送 | generationが同じでもlease/attempt/revisionの不一致を捨て、状態を後戻りさせない | 自動 |
| RT-GEN-03 | P0 | dockを閉じる、controllerを破棄する、またはOBS終了処理を開始した後にqueued callbackを配送 | receiver/lifetime guardによりcallbackがUIやcontrollerへアクセスしない。use-after-freeがない | 自動＋ASan |

### 5.3 `EXIT` とプラグイン寿命

| ID | 優先度 | 前提と操作 | 合格条件 | 区分 |
|---|---|---|---|---|
| RT-EXIT-01 | P0 | YouTube Streaming中に`OBS_FRONTEND_EVENT_EXIT`を配送 | callback内でFrontend callbackとdockを各1回だけ解除し、UI操作を無効化する。API callbackから戻った後にFrontend APIを呼ばない | 自動＋手動 |
| RT-EXIT-02 | P0 | YouTube Connecting中、stop/release処理中、native同期拒否直後にEXITする | callback contextとreaperが完了まで生存し、plugin DLLが先にunloadされない。OBS終了がハングせず、未配送callbackがコードへ入らない | 自動＋手動＋ASan |
| RT-EXIT-03 | P1 | EXIT、module unload、重複shutdownを連続して呼ぶ | callback/dock/remove/releaseがidempotentで、二重解除・二重freeがない | 自動 |

### 5.4 入力検証・設定・資格情報

| ID | 優先度 | 前提と操作 | 合格条件 | 区分 |
|---|---|---|---|---|
| RT-INPUT-01 | P1 | native service id/providerがTwitch以外、またはcustom RTMP・不明provider | YouTube outputを開始しない。URLやkeyの形からTwitchと推測しない | 自動＋手動 |
| RT-INPUT-02 | P0 | 映像encoderが未設定、音声encoderが未設定、映像trackが複数、またはTwitch VOD用の第2音声trackがある | 欠落・複数映像はYouTube試行だけを安全に拒否する。VOD音声trackは許可し、main audio index 0だけをYouTubeへ送る。native Twitchは継続する | 自動＋手動 |
| RT-INPUT-03 | P0 | H.264/AAC以外の第一実装対象外codec（AV1、HEVC、未対応HDR）を設定 | YouTubeへ接続せず、nativeのencoder設定を変更・再スケールしない。ユーザー向けエラーはcodec名など非秘密情報だけ | 自動＋手動 |
| RT-INPUT-04 | P0 | URLが空、schemeがRTMPS/RTMP以外、hostなし、上限超過、keyをquery/pathに連結した不正URL | output/serviceを作らず、YouTubeをSetupRequired/Failedにする。native Twitchは継続 | 自動 |
| RT-INPUT-05 | P0 | Credential Managerが未登録、read access denied、OSエラー、型不正、サイズ不正を返す | YouTube outputを作らず、資格情報の状態を表示する。Twitchを停止しない。秘密値を返り値・ログへ出さない | 自動＋手動 |
| RT-INPUT-06 | P1 | Start直前にkeyを削除・resetし、古い設定を残した状態で開始 | 直前のread結果だけを使い、古いkeyを再利用しない。失敗後に自動的な無限retryをしない | 自動＋手動 |
| RT-INPUT-07 | P1 | profile configがnull、設定のfuture schema、保存失敗、profile切り替え中に設定を読む | config pointerや`config_get_string()`の返却ポインターを保持しない。安全な未設定状態へ遷移し、native出力を触らない | 自動＋手動 |

### 5.5 所有権、signal、release順序

| ID | 優先度 | 前提と操作 | 合格条件 | 区分 |
|---|---|---|---|---|
| RT-OWN-01 | P0 | native output、video encoder、audio encoderを取得してYouTube outputへ接続し、開始から停止まで実行 | adapterがnative outputとencoderの一時refを保持する。`obs_output_set_*_encoder()`後のrefを正しく解放し、native encoderを変更・破棄しない | 自動fake＋ASan |
| RT-OWN-02 | P0 | `obs_output_set_service()`後にadapter側のservice refを早期releaseするfaultを注入 | serviceはYouTube outputの破棄・signal disconnect完了まで生存する。service use-after-freeがない | 自動fake＋ASan |
| RT-OWN-03 | P0 | output callback内でstop/releaseが再入する順序、`stop` signal中のdestroyを試す | callback内では最終releaseしない。signal callbackから戻った後にdisconnect→output release→encoder/service/native ref releaseの順で完了する | 自動fake＋ASan |
| RT-OWN-04 | P0 | start失敗、service作成失敗、encoder attach失敗、signal connect失敗を各作成段階で注入 | 途中まで作成した全resourceを一度だけ解放し、漏れ・double release・native停止がない | 自動fake＋ASan |
| RT-OWN-05 | P0 | 旧leaseをretire中に新leaseを作成できるようなイベント順を注入 | 完全release通知が1回届くまで新leaseを実際のoutputへ渡さない。output/callback contextを共有しない | 自動fake |
| RT-OWN-06 | P1 | `reconnect`、`reconnect_success`、`stopping`、`stop`、`deactivate`を逆順・重複配送 | 状態変更は現leaseだけを受理し、releaseのterminal通知は1回。stale signalで再接続を開始しない | 自動 |

### 5.6 RTMPS URL、key、ログ

| ID | 優先度 | 前提と操作 | 合格条件 | 区分 |
|---|---|---|---|---|
| RT-SECRET-01 | P0 | 有効なテストkeyを含むservice settingsを作成し、start/stop/error/reconnectを通す | OBS log、plugin log、Qtメッセージ、例外、error text、snapshot、test traceにkeyが一度も出ない | 自動文字列検査＋手動 |
| RT-SECRET-02 | P0 | server URLとkeyを結合したRTMP(S)文字列、query/pathへkeyを入れた入力を渡す | ログやエラーメッセージへ完全URLを出さない。保存・診断ではserver hostなど非秘密の識別子だけを使う | 自動 |
| RT-SECRET-03 | P0 | start失敗、credential failure、URL不正、YouTube切断のログを収集 | key、refresh token、authorization code、Credential Manager blobがなく、エラーコードと安全な状態だけが記録される | 自動＋手動 |
| RT-SECRET-04 | P0 | テスト後にprofile、scene collection、safe-save出力、portable profile export、crash artifactをscanする | `[EasyMultistream]`や診断成果物にkeyが存在しない。既知の旧plaintext keyがあれば移行処理の要件どおりに除去される | 自動文字列検査 |

## 6. 手動OBS 32.2.2テスト

手動試験は、fakeハーネスで合格した後に行います。実配信は短時間・限定公開または非公開で行い、確認後に必ずStopし、YouTube側の配信が終了したことを確認します。

| ID | 操作 | 確認すること |
|---|---|---|
| OBS-M-00 | cleanなOBSユーザー設定で初回起動し、案内を確認してdockを閉じ、OBSを再起動する | 初回だけEasy Multistream dockが自動表示され、未設定時の案内が読める。2回目は勝手に再表示せず、Docksメニューから再表示できる。profile切替・dock表示/非表示で配信状態は変わらない |
| OBS-M-01 | テストprofileでTwitch nativeを設定し、Easy Multistreamを有効化。YouTube RTMPS server/keyを保存してOBSのStart Streamingを1回押す | TwitchとYouTubeがそれぞれ接続する。OBSのnative Start/Stopボタンを増やさない。YouTubeが接続中の間に送信中と表示しない |
| OBS-M-02 | OBS-M-01の状態でYouTube outputだけをテストfaultまたは接続失敗にする | YouTubeだけが失敗し、Twitchは送信を続ける。再接続ループやnative Stopがない |
| OBS-M-03 | OBS-M-01でStopを押し、Twitch停止・YouTube停止・YouTube完全releaseの順序をログで確認する | 停止後にOBSを閉じてもクラッシュせず、次回Startで旧outputが残っていない |
| OBS-M-04 | YouTube実配信ではなくfake/local test outputで、Start→Stopを完全release待ちなしで短時間に20回程度繰り返す。可能なら各回でStart/Stopイベントを遅延させる（実YouTubeでの反復は必要最小限の数回に留める） | outputが重複せず、映像停止・クラッシュ・OBS UIハング・ハンドルやスレッドの増加がない |
| OBS-M-05 | YouTube Connecting、Streaming、Stoppingの各状態でprofileを切り替える | 旧profileのYouTube outputが完全releaseされ、新profileの設定・keyでのみ次回開始する。旧状態がdockを上書きしない |
| OBS-M-06 | YouTube Connecting、Streaming、Stoppingの各状態でOBSを終了する | dock/callbackが一度だけ解除され、OBS終了がハングせず、再起動後に古い状態が残らない |
| OBS-M-07 | 事前にnativeのcodec/audio trackを対象外設定へ変更してStartする | YouTubeだけが設定不足として拒否され、native Twitchは変更されない。復元後は正常に開始できる |
| OBS-M-08 | keyをRemoveしてからStart、またはCredential Managerを一時的に利用不可にしてStart | SetupRequired/Unavailable相当の表示になり、keyがログや設定ファイルへ現れず、Twitchは停止しない |
| OBS-M-09 | Start中またはStreaming中にYouTube接続だけを切断する | YouTubeの状態が明確に失敗・再接続中・停止のいずれかになり、Twitchが継続する。停止後に次回Startできる |
| OBS-M-10 | OBSのDocksメニューからdockを閉じ、再表示する | dockの表示・非表示だけが変わり、配信状態とoutput寿命は変わらない |
| OBS-M-11 | OBS-M-01を専用のportable OBS 32.2.2でも繰り返す | 通常版とは別のprofile・credential境界で動作し、普段のOBS環境に設定を残さない |

### 実配信後の確認

- TwitchとYouTubeの両方で映像・音声が短時間でも確認できる。
- YouTubeの配信を限定公開または非公開で作成し、配信終了後にアーカイブが終了する。
- YouTubeのLive Control Roomの接続状態とOBS dockの状態が矛盾しない。
- YouTubeのDual streamを使う場合は、このランタイムゲートとは別に横配信の自動縦クロップを確認する。縦用の個別Encoder出力は、別機能として追加するまで必須ゲートにしない。

## 7. 既存自動テストとの対応

現在のCTestは状態機械・bridge境界・UIを検証しますが、実際のOBS RTMPS出力は生成しません。そのため次の項目を保証するものではありません。

| 既存テスト | 現在確認できる範囲 | ランタイムで追加が必要な範囲 |
|---|---|---|
| unit | profile設定、safe-save、秘密値検証、Credential Manager fake、SecureBuffer | 実OBS output/service、RTMPS、callback thread、URL・ログscan |
| session | native分類、開始受理、native同期失敗、YouTube開始/停止、失敗隔離、rapid restart、stale lease、profile generation、EXIT terminal | Frontend callback、OBS 32.2.2のsignal順序、実outputのteardown、スレッド・reaper寿命 |
| runtime | native開始確認、YouTube開始受理とstart通知の分離、失敗隔離、明示retry、rapid restart、stale callback、profile generation、EXIT fallback | 実OBS signal thread、service/output生成失敗、RTMP接続、reaper join |
| UI | dock状態、実ランタイムsnapshot表示、配信中の設定保護、YouTube失敗表示とretry、password editor、key削除確認 | 実切断・profile change・終了中のOBS widget寿命 |
| locales | en-US/ja-JPキーの整合性 | 実エラー状態の翻訳と秘密情報を含まない表示 |

最低限のローカル自動確認は次のコマンドです。

```text
ctest --test-dir <repository-local-build> --output-on-failure
```

テスト結果にはbuild directoryの絶対パスやCredential Managerの実target、Stream keyを含めず、コミットIDとOBS 32.2.2の識別子だけを残します。

## 8. 所有権・終了順序の証跡

各ランタイムテストでは、秘密値ではなく次の値だけをイベントトレースへ記録します。

```text
event, thread/owner, generation, native_attempt, youtube_attempt,
effect, output_signal, stop_requested, signal_disconnected,
output_released, video_encoder_released, audio_encoder_released,
service_released, native_output_released, callback_context_released
```

正常な終了の期待順序は次のとおりです。

```text
generation invalidation / new work拒否
  → YouTube outputへstop要求を1回
  → OBS signal callbackからreturn
  → signal disconnect
  → YouTube output release
  → adapter-held encoder ref release
  → YouTube service release
  → native output ref release
  → callback context release
  → youtubeReleased(lease)を1回通知
```

`stop` signalを「完全破棄完了」とみなしてはいけません。新しいStartの許可点は、上記のrelease順序を終えて`youtubeReleased`が届いた時点です。これは[OBS output integration contract](runtime-output-design.md)の所有権・signal規則と一致していなければなりません。

## 9. 実装後の出荷チェックリスト

- [x] OBS 32.2.2固定環境でwarnings-as-errorsビルドできる。
- [x] 現在のCTestがすべて通る。
- [x] start acceptedとstart signalを分けた状態機械テストが通る。
- [x] リポジトリ内の隔離portable OBS 32.2.2で、0.1.0基礎buildのload、Startup complete、clean unload、memory leaks 0を確認した。
- [ ] 0.2.0で初回dock表示、表示済みmarker保存、2回目の非表示、トレイ復帰後の表示を確認した。
- [ ] `obs_output_start()`同期拒否とnative StartStreaming同期拒否を別々にテストした。
- [ ] output error、YouTube-only disconnect、credential failure、missing URL/key、unsupported codecをテストした。
- [ ] stop、duplicate stop、rapid stop-start、profile change、EXITをテストした。
- [ ] stale generation/lease/revisionのcallbackを破棄できる。
- [ ] encoder/service/output/callback contextのrelease順序をtraceとASan等で確認した。
- [ ] RTMPS URL、Stream key、Credential Manager blobがログ・profile・export・diagnosticへ出ない。
- [ ] native Twitchの設定、encoder、Start/Stop状態がYouTube失敗で変化しない。
- [ ] 実配信は専用テストアカウントで完了し、credentialと一時profileを削除した。
- [ ] 未実施項目・既知のP2・waiverをリリース記録へ明記した。
