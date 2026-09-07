// ============================================================
// win_handle.h — Windows HANDLE 的最小 RAII 封装
//
// UniqueHandle 只拥有并关闭内核句柄，不附带具体 API 的业务语义。调用方
// 若需要把句柄交给另一个所有者，应显式调用 Release()，避免重复关闭。
// ============================================================
#pragma once

#include <windows.h>

namespace win
{
    // ============================================================
    // UniqueHandle — HANDLE 所有权包装
    // ============================================================
    // 只负责拥有并关闭一个 Windows 内核句柄。
    // INVALID_HANDLE_VALUE 和 nullptr 都表示“没有句柄”。
    class UniqueHandle
    {
    public:
        // 默认构造表示当前对象不拥有任何句柄。
        UniqueHandle() = default;

        // 从调用方接管句柄所有权；传入无效句柄也保持安全。
        explicit UniqueHandle(HANDLE handle) : m_handle(handle) {}

        ~UniqueHandle()
        {
            // 作用域结束时自动关闭仍由对象持有的句柄。
            Reset();
        }

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept
            : m_handle(other.Release())
        {
            // 移动构造只转移所有权，不复制底层 HANDLE。
        }

        UniqueHandle& operator=(UniqueHandle&& other) noexcept
        {
            if (this != &other)
            {
                // 先释放当前句柄，再接管源对象的句柄。
                Reset(other.Release());
            }
            return *this;
        }

        // 借用句柄，不转移所有权。
        HANDLE Get() const noexcept { return m_handle; }

        explicit operator bool() const noexcept
        {
            // Windows 同时使用 nullptr 和 INVALID_HANDLE_VALUE 表示失败，两个都要排除。
            return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
        }

        // 放弃所有权并返回原句柄；调用方随后负责关闭它。
        HANDLE Release() noexcept
        {
            HANDLE handle = m_handle;
            m_handle = INVALID_HANDLE_VALUE;
            return handle;
        }

        void Reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        {
            // Reset 可用于关闭当前句柄并替换为新句柄。
            if (*this)
            {
                CloseHandle(m_handle);
            }
            m_handle = handle;
        }

    private:
        HANDLE m_handle = INVALID_HANDLE_VALUE;
    };
}
