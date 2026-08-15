using System;
using System.Threading.Tasks;
using Windows.Media.Miracast;

namespace MiracastConsoleTest
{
    /// <summary>
    /// 最小验证:桌面进程能否完整跑通 Miracast 接收器(无 UWP 宿主)。
    /// 若 Start=Success 且广播可被搜到,则证明桌面进程方案可行。
    /// </summary>
    internal class Program
    {
        private static async Task<int> Main(string[] args)
        {
            try
            {
                Console.WriteLine("[test] creating MiracastReceiver...");
                var receiver = new MiracastReceiver();

                var settings = receiver.GetDefaultSettings();
                settings.FriendlyName += " MirrorCenterTest";
                settings.AuthorizationMethod = MiracastReceiverAuthorizationMethod.None;
                settings.RequireAuthorizationFromKnownTransmitters = false;

                Console.WriteLine("[test] ApplySettings...");
                var apply = await receiver.DisconnectAllAndApplySettingsAsync(settings);
                Console.WriteLine($"[test] Apply={apply.Status}");

                Console.WriteLine("[test] CreateSession...");
                var session = await receiver.CreateSessionAsync(null);
                session.AllowConnectionTakeover = true;
                session.ConnectionCreated += (s, e) =>
                    Console.WriteLine($"[test] ConnectionCreated {e.Connection.Transmitter.Name}");
                session.Disconnected += (s, e) =>
                    Console.WriteLine($"[test] Disconnected {e.Connection.Transmitter.Name}");
                session.MediaSourceCreated += (s, e) =>
                    Console.WriteLine($"[test] MediaSourceCreated");

                Console.WriteLine("[test] Start...");
                var start = await session.StartAsync();
                Console.WriteLine($"[test] Start={start.Status}");

                Console.WriteLine("[test] listening, waiting 45s for a cast...");
                await Task.Delay(45000);
                Console.WriteLine("[test] done");
                return 0;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[test] FAIL: {ex}");
                return 1;
            }
        }
    }
}
