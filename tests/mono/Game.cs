// The scripts of a game for the engine commands of the remote control on a
// real Mono runtime: the test hosts embed Mono, load this assembly, call
// Game.Boot.Spawn(), then check the commands with CheckRealMono() of
// tests/remote_engine.h. Compiled with mcs -target:library -out:Game.dll.
using System;

namespace Game
{
    public enum Team { Red = 1, Blue = 2 }

    public struct Vec3
    {
        public float x, y, z;

        public Vec3(float x, float y, float z)
        {
            this.x = x;
            this.y = y;
            this.z = z;
        }
    }

    public class Player
    {
        public const int MaxHealth = 1000;
        public static int s_Count;
        public static Player s_Local;

        public int health = 100;
        public float speed = 4.5f;
        public bool alive = true;
        public Team team = Team.Blue;
        public string playerName = "Alice";
        public Player target;
        public Vec3 spawn = new Vec3(1, 2, 3);

        public static int Add(int a, int b) { return a + b; }

        public void Heal(int amount) { health += amount; }

        public string GetName() { return playerName; }

        public virtual string Kind() { return "player"; }

        public static void Fail() { throw new InvalidOperationException("it failed on purpose"); }
    }

    public class Boss : Player
    {
        public int phase = 1;

        public override string Kind() { return "boss"; }
    }

    public static class Boot
    {
        public static Boss s_Boss;
        public static int s_MainThread;

        // The thread a call runs on.
        public static int Thread() { return System.Threading.Thread.CurrentThread.ManagedThreadId; }

        public static void Spawn()
        {
            s_MainThread = Thread();
            var alice = new Player();
            var bob = new Player { playerName = "Bob", health = 80 };
            alice.target = bob;
            Player.s_Local = alice;
            Player.s_Count = 2;
            s_Boss = new Boss { playerName = "Boss", health = 500 };
            // Out of the nursery of SGen, which moves the objects it keeps.
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
        }
    }
}
