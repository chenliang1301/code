```mermaid
flowchart TD
    Start --> |创建线程| Reader1
    Start --> |创建线程| Reader2
    Start --> |创建线程| Writer
    Reader1 -->|读锁| Cache
    Reader2 -->|读锁| Cache
    Writer -->|写锁| Cache
    Cache --> End
```