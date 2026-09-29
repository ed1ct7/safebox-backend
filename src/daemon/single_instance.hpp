// Один экземпляр окна на порт: именованный мьютекс + поиск окна первого экземпляра (только Windows)
#pragma once

namespace safebox::daemon {

class SingleInstance {
public:
    // port != 0: у режима "любой свободный порт" нет общего адреса, ограничивать нечего.
    explicit SingleInstance(int port);
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // false - мьютекс уже держит другой процесс.
    [[nodiscard]] bool isFirst() const noexcept { return first_; }

    // Выводит окно первого экземпляра на передний план. Тот мог только запуститься, поэтому
    // окно ждем до нескольких секунд; false - окна так и нет.
    [[nodiscard]] bool activateExisting() const;

private:
    void* mutex_ = nullptr; // HANDLE
    int port_ = 0;
    bool first_ = false;
};

} // namespace safebox::daemon
