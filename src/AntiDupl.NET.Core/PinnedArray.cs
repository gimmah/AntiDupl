using System;
using System.Runtime.InteropServices;

namespace AntiDupl.NET.Core
{
    /// <summary>
    /// Pins a managed array for the lifetime of the object so that its address can be
    /// safely handed to native code.
    ///
    /// Marshal.UnsafeAddrOfPinnedArrayElement does NOT pin anything: if the array is not
    /// pinned, the GC is free to move or even collect it (once the array is no longer
    /// referenced) while the native code is still reading or writing it, which crashes
    /// the process with an access violation inside AntiDupl.dll.
    /// Always use: using (var pin = new PinnedArray(array)) { native(pin.Address); }
    /// </summary>
    public sealed class PinnedArray : IDisposable
    {
        private GCHandle m_handle;

        /// <summary>Pins the given array until Dispose is called.</summary>
        public PinnedArray(Array array)
        {
            if (array == null)
                throw new ArgumentNullException(nameof(array));
            m_handle = GCHandle.Alloc(array, GCHandleType.Pinned);
        }

        /// <summary>Address of the first element.</summary>
        public IntPtr Address
        {
            get { return m_handle.AddrOfPinnedObject(); }
        }

        /// <summary>Address located byteOffset bytes after the first element.</summary>
        public IntPtr At(int byteOffset)
        {
            return IntPtr.Add(m_handle.AddrOfPinnedObject(), byteOffset);
        }

        /// <summary>Unpins the array.</summary>
        public void Dispose()
        {
            if (m_handle.IsAllocated)
                m_handle.Free();
        }
    }
}
