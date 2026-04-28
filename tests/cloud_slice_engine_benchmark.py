#!/usr/bin/env python3
"""
OrcaSlicer Cloud Engine Benchmark Suite
========================================

This script validates whether the one-shot execution model meets cloud deployment requirements.

Test Categories:
1. Performance: Startup time, slicing time, memory usage
2. Reliability: Error handling, recovery, stability under load
3. Scalability: Concurrent tasks, resource contention
4. Integration: API compatibility, output validation

Usage:
    python3 cloud_slice_engine_benchmark.py --engine ./orca-slice-engine --model test.3mf

Output:
    - JSON report: benchmark_results.json
    - Human-readable summary
"""

import argparse
import subprocess
import time
import os
import sys
import json
import tempfile
import shutil
import threading
import queue
import psutil
import statistics
from datetime import datetime
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, asdict
from typing import List, Optional, Dict, Any
import hashlib


@dataclass
class BenchmarkResult:
    """Single benchmark result."""
    test_name: str
    success: bool
    duration_ms: float
    memory_peak_mb: float
    exit_code: int
    error_message: str = ""
    details: Dict[str, Any] = None


@dataclass
class BenchmarkSuite:
    """Collection of benchmark results."""
    timestamp: str
    engine_path: str
    model_path: str
    system_info: Dict[str, Any]
    results: List[BenchmarkResult]
    summary: Dict[str, Any]


class ProcessMonitor:
    """Monitor process resource usage."""
    
    def __init__(self, pid: int, interval: float = 0.1):
        self.pid = pid
        self.interval = interval
        self.memory_samples = []
        self.cpu_samples = []
        self._stop = threading.Event()
        self._thread = None
    
    def start(self):
        self._thread = threading.Thread(target=self._monitor)
        self._thread.start()
    
    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join()
    
    def _monitor(self):
        try:
            proc = psutil.Process(self.pid)
            while not self._stop.is_set():
                try:
                    self.memory_samples.append(proc.memory_info().rss / 1024 / 1024)  # MB
                    self.cpu_samples.append(proc.cpu_percent())
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    break
                time.sleep(self.interval)
        except Exception:
            pass
    
    @property
    def peak_memory_mb(self) -> float:
        return max(self.memory_samples) if self.memory_samples else 0
    
    @property
    def avg_cpu_percent(self) -> float:
        return statistics.mean(self.cpu_samples) if self.cpu_samples else 0


class CloudEngineBenchmark:
    """Benchmark suite for cloud slicing engine."""
    
    def __init__(self, engine_path: str, model_path: str, resources_path: str = None):
        self.engine_path = engine_path
        self.model_path = model_path
        self.resources_path = resources_path or self._find_resources()
        self.results: List[BenchmarkResult] = []
        
        # Validate paths
        if not os.path.exists(engine_path):
            raise FileNotFoundError(f"Engine not found: {engine_path}")
        if not os.path.exists(model_path):
            raise FileNotFoundError(f"Model not found: {model_path}")
    
    def _find_resources(self) -> str:
        """Find resources directory relative to engine."""
        engine_dir = os.path.dirname(self.engine_path)
        candidates = [
            os.path.join(engine_dir, "resources"),
            os.path.join(engine_dir, "..", "resources"),
            "./resources"
        ]
        for path in candidates:
            if os.path.exists(path):
                return os.path.abspath(path)
        return None
    
    def _run_slice(self, output_path: str, plate_id: int = None, 
                   timeout: int = 600) -> BenchmarkResult:
        """Execute a single slicing task."""
        cmd = [self.engine_path, self.model_path, "-o", output_path]
        
        if self.resources_path:
            cmd.extend(["-r", self.resources_path])
        if plate_id:
            cmd.extend(["-p", str(plate_id)])
        
        start_time = time.time()
        process = None
        monitor = None
        
        try:
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )
            
            # Start monitoring
            monitor = ProcessMonitor(process.pid)
            monitor.start()
            
            # Wait for completion with timeout
            stdout, stderr = process.communicate(timeout=timeout)
            
            duration_ms = (time.time() - start_time) * 1000
            
            return BenchmarkResult(
                test_name="single_slice",
                success=process.returncode in (0, 7),
                duration_ms=duration_ms,
                memory_peak_mb=monitor.peak_memory_mb if monitor else 0,
                exit_code=process.returncode,
                error_message=stderr if process.returncode not in (0, 7) else "",
                details={"stdout": stdout[:1000], "stderr": stderr[:1000]}
            )
            
        except subprocess.TimeoutExpired:
            if process:
                process.kill()
            return BenchmarkResult(
                test_name="single_slice",
                success=False,
                duration_ms=timeout * 1000,
                memory_peak_mb=monitor.peak_memory_mb if monitor else 0,
                exit_code=-1,
                error_message=f"Timeout after {timeout}s"
            )
        except Exception as e:
            return BenchmarkResult(
                test_name="single_slice",
                success=False,
                duration_ms=(time.time() - start_time) * 1000,
                memory_peak_mb=0,
                exit_code=-1,
                error_message=str(e)
            )
        finally:
            if monitor:
                monitor.stop()
    
    # ==================== Test Cases ====================
    
    def test_startup_time(self, iterations: int = 10) -> BenchmarkResult:
        """Test 1: Measure process startup overhead."""
        times = []
        
        for i in range(iterations):
            start = time.time()
            process = subprocess.Popen(
                [self.engine_path, "--help"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )
            process.communicate()
            times.append((time.time() - start) * 1000)
        
        return BenchmarkResult(
            test_name="startup_time",
            success=True,
            duration_ms=statistics.mean(times),
            memory_peak_mb=0,
            exit_code=0,
            details={
                "iterations": iterations,
                "min_ms": min(times),
                "max_ms": max(times),
                "median_ms": statistics.median(times),
                "stddev_ms": statistics.stdev(times) if len(times) > 1 else 0
            }
        )
    
    def test_single_slice_performance(self, iterations: int = 5) -> BenchmarkResult:
        """Test 2: Single slicing task performance."""
        results = []
        
        with tempfile.TemporaryDirectory() as tmpdir:
            for i in range(iterations):
                output = os.path.join(tmpdir, f"output_{i}")
                result = self._run_slice(output)
                results.append(result)
        
        durations = [r.duration_ms for r in results]
        memories = [r.memory_peak_mb for r in results]
        successes = [r.success for r in results]
        
        return BenchmarkResult(
            test_name="single_slice_performance",
            success=all(successes),
            duration_ms=statistics.mean(durations),
            memory_peak_mb=max(memories),
            exit_code=0 if all(successes) else 1,
            details={
                "iterations": iterations,
                "duration_min_ms": min(durations),
                "duration_max_ms": max(durations),
                "duration_median_ms": statistics.median(durations),
                "memory_avg_mb": statistics.mean(memories),
                "success_rate": sum(successes) / len(successes)
            }
        )
    
    def test_concurrent_slicing(self, workers: int = 4, tasks_per_worker: int = 2) -> BenchmarkResult:
        """Test 3: Concurrent slicing tasks."""
        total_tasks = workers * tasks_per_worker
        results = []
        start_time = time.time()
        
        def slice_task(task_id: int):
            with tempfile.TemporaryDirectory() as tmpdir:
                output = os.path.join(tmpdir, f"output_{task_id}")
                return self._run_slice(output)
        
        with ThreadPoolExecutor(max_workers=workers) as executor:
            futures = [executor.submit(slice_task, i) for i in range(total_tasks)]
            for future in as_completed(futures):
                results.append(future.result())
        
        total_duration_ms = (time.time() - start_time) * 1000
        successes = [r.success for r in results]
        durations = [r.duration_ms for r in results]
        memories = [r.memory_peak_mb for r in results]
        
        return BenchmarkResult(
            test_name="concurrent_slicing",
            success=all(successes),
            duration_ms=total_duration_ms,
            memory_peak_mb=sum(memories),  # Total memory across all workers
            exit_code=0 if all(successes) else 1,
            details={
                "workers": workers,
                "total_tasks": total_tasks,
                "tasks_per_worker": tasks_per_worker,
                "success_rate": sum(successes) / len(successes),
                "avg_task_duration_ms": statistics.mean(durations),
                "throughput_tasks_per_min": total_tasks / (total_duration_ms / 60000)
            }
        )
    
    def test_memory_cleanup(self, iterations: int = 10) -> BenchmarkResult:
        """Test 4: Verify memory is released after each execution."""
        memory_before = psutil.Process().memory_info().rss / 1024 / 1024
        peak_memories = []
        
        with tempfile.TemporaryDirectory() as tmpdir:
            for i in range(iterations):
                output = os.path.join(tmpdir, f"output_{i}")
                result = self._run_slice(output)
                peak_memories.append(result.memory_peak_mb)
                
                # Force garbage collection
                import gc
                gc.collect()
        
        memory_after = psutil.Process().memory_info().rss / 1024 / 1024
        memory_leak = memory_after - memory_before
        
        return BenchmarkResult(
            test_name="memory_cleanup",
            success=memory_leak < 50,  # Less than 50MB leak
            duration_ms=sum(peak_memories),
            memory_peak_mb=max(peak_memories),
            exit_code=0,
            details={
                "iterations": iterations,
                "memory_before_mb": memory_before,
                "memory_after_mb": memory_after,
                "memory_leak_mb": memory_leak,
                "peak_memories_mb": peak_memories
            }
        )
    
    def test_error_recovery(self) -> BenchmarkResult:
        """Test 5: Error handling and recovery."""
        test_cases = [
            ("invalid_file", "/nonexistent/file.3mf", "File not found"),
            ("invalid_plate", self.model_path, "Invalid plate", ["-p", "999"]),
        ]
        
        results = []
        for name, model, expected_error, *extra_args in test_cases:
            with tempfile.TemporaryDirectory() as tmpdir:
                output = os.path.join(tmpdir, "output")
                cmd = [self.engine_path, model, "-o", output]
                if extra_args:
                    cmd.extend(extra_args[0])
                
                start = time.time()
                process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                stdout, stderr = process.communicate()
                duration = (time.time() - start) * 1000
                
                results.append({
                    "case": name,
                    "exit_code": process.returncode,
                    "expected_error": expected_error,
                    "has_error": process.returncode != 0,
                    "duration_ms": duration
                })
        
        return BenchmarkResult(
            test_name="error_recovery",
            success=all(r["has_error"] for r in results),
            duration_ms=sum(r["duration_ms"] for r in results),
            memory_peak_mb=0,
            exit_code=0,
            details={"cases": results}
        )
    
    def test_output_validation(self) -> BenchmarkResult:
        """Test 6: Validate output file integrity."""
        with tempfile.TemporaryDirectory() as tmpdir:
            output = os.path.join(tmpdir, "output")
            result = self._run_slice(output)
            
            if not result.success:
                return BenchmarkResult(
                    test_name="output_validation",
                    success=False,
                    duration_ms=result.duration_ms,
                    memory_peak_mb=result.memory_peak_mb,
                    exit_code=result.exit_code,
                    error_message="Slicing failed"
                )
            
            # Check output file
            output_file = f"{output}.gcode.3mf"
            if not os.path.exists(output_file):
                return BenchmarkResult(
                    test_name="output_validation",
                    success=False,
                    duration_ms=result.duration_ms,
                    memory_peak_mb=result.memory_peak_mb,
                    exit_code=-1,
                    error_message=f"Output file not found: {output_file}"
                )
            
            file_size = os.path.getsize(output_file)
            
            # Basic validation: check if it's a valid ZIP (3MF is ZIP-based)
            import zipfile
            is_valid_zip = zipfile.is_zipfile(output_file)
            
            return BenchmarkResult(
                test_name="output_validation",
                success=is_valid_zip and file_size > 0,
                duration_ms=result.duration_ms,
                memory_peak_mb=result.memory_peak_mb,
                exit_code=0,
                details={
                    "output_file": output_file,
                    "file_size_bytes": file_size,
                    "is_valid_3mf": is_valid_zip
                }
            )
    
    def test_load_stress(self, duration_seconds: int = 60, max_workers: int = 8) -> BenchmarkResult:
        """Test 7: Sustained load test."""
        completed_tasks = 0
        failed_tasks = 0
        start_time = time.time()
        lock = threading.Lock()
        
        def stress_task():
            nonlocal completed_tasks, failed_tasks
            with tempfile.TemporaryDirectory() as tmpdir:
                output = os.path.join(tmpdir, "output")
                result = self._run_slice(output, timeout=120)
                with lock:
                    if result.success:
                        completed_tasks += 1
                    else:
                        failed_tasks += 1
                return result
        
        results = []
        with ThreadPoolExecutor(max_workers=max_workers) as executor:
            while time.time() - start_time < duration_seconds:
                future = executor.submit(stress_task)
                results.append(future)
                time.sleep(0.5)  # Submit tasks at 2/sec rate
        
        # Wait for all to complete
        for future in as_completed(results):
            pass
        
        actual_duration = time.time() - start_time
        
        return BenchmarkResult(
            test_name="load_stress",
            success=failed_tasks == 0,
            duration_ms=actual_duration * 1000,
            memory_peak_mb=0,
            exit_code=0 if failed_tasks == 0 else 1,
            details={
                "duration_seconds": duration_seconds,
                "max_workers": max_workers,
                "completed_tasks": completed_tasks,
                "failed_tasks": failed_tasks,
                "throughput_per_min": completed_tasks / (actual_duration / 60)
            }
        )
    
    # ==================== Main Runner ====================
    
    def run_all_tests(self) -> BenchmarkSuite:
        """Run all benchmark tests."""
        print("=" * 60)
        print("OrcaSlicer Cloud Engine Benchmark Suite")
        print("=" * 60)
        print(f"Engine: {self.engine_path}")
        print(f"Model: {self.model_path}")
        print(f"Resources: {self.resources_path}")
        print("=" * 60)
        
        tests = [
            ("Startup Time", lambda: self.test_startup_time()),
            ("Single Slice Performance", lambda: self.test_single_slice_performance()),
            ("Concurrent Slicing (4 workers)", lambda: self.test_concurrent_slicing(4, 2)),
            ("Memory Cleanup", lambda: self.test_memory_cleanup()),
            ("Error Recovery", lambda: self.test_error_recovery()),
            ("Output Validation", lambda: self.test_output_validation()),
            # ("Load Stress (60s)", lambda: self.test_load_stress(60, 8)),  # Optional long test
        ]
        
        for name, test_func in tests:
            print(f"\nRunning: {name}...")
            try:
                result = test_func()
                self.results.append(result)
                status = "✓ PASS" if result.success else "✗ FAIL"
                print(f"  {status} - {result.duration_ms:.0f}ms, Peak Memory: {result.memory_peak_mb:.1f}MB")
                if not result.success and result.error_message:
                    print(f"  Error: {result.error_message[:100]}")
            except Exception as e:
                print(f"  ✗ ERROR - {str(e)}")
                self.results.append(BenchmarkResult(
                    test_name=name.lower().replace(" ", "_"),
                    success=False,
                    duration_ms=0,
                    memory_peak_mb=0,
                    exit_code=-1,
                    error_message=str(e)
                ))
        
        return self._generate_report()
    
    def _generate_report(self) -> BenchmarkSuite:
        """Generate final report."""
        passed = sum(1 for r in self.results if r.success)
        total = len(self.results)
        
        summary = {
            "total_tests": total,
            "passed": passed,
            "failed": total - passed,
            "pass_rate": passed / total if total > 0 else 0,
            "total_duration_ms": sum(r.duration_ms for r in self.results),
            "max_memory_mb": max(r.memory_peak_mb for r in self.results),
            "recommendations": self._generate_recommendations()
        }
        
        suite = BenchmarkSuite(
            timestamp=datetime.now().isoformat(),
            engine_path=self.engine_path,
            model_path=self.model_path,
            system_info={
                "platform": sys.platform,
                "python_version": sys.version,
                "cpu_count": os.cpu_count(),
                "total_memory_gb": psutil.virtual_memory().total / (1024**3)
            },
            results=self.results,
            summary=summary
        )
        
        # Print summary
        print("\n" + "=" * 60)
        print("BENCHMARK SUMMARY")
        print("=" * 60)
        print(f"Tests: {passed}/{total} passed ({summary['pass_rate']*100:.0f}%)")
        print(f"Total Duration: {summary['total_duration_ms']/1000:.1f}s")
        print(f"Peak Memory: {summary['max_memory_mb']:.1f}MB")
        print("\nRecommendations:")
        for rec in summary["recommendations"]:
            print(f"  • {rec}")
        
        return suite
    
    def _generate_recommendations(self) -> List[str]:
        """Generate recommendations based on test results."""
        recommendations = []
        
        for result in self.results:
            if not result.success:
                if "startup" in result.test_name:
                    recommendations.append("Startup time is slow - consider process pooling")
                elif "memory" in result.test_name:
                    recommendations.append("Memory leak detected - investigate process lifecycle")
                elif "concurrent" in result.test_name:
                    recommendations.append("Concurrent execution issues - reduce worker count")
            else:
                if result.duration_ms > 30000:  # > 30s
                    recommendations.append(f"{result.test_name}: Consider timeout > 60s for large models")
                if result.memory_peak_mb > 2000:  # > 2GB
                    recommendations.append(f"{result.test_name}: High memory usage - allocate > 4GB per worker")
        
        if not recommendations:
            recommendations.append("All tests passed - one-shot execution model is suitable for cloud deployment")
        
        return recommendations


def main():
    parser = argparse.ArgumentParser(description="OrcaSlicer Cloud Engine Benchmark")
    parser.add_argument("--engine", required=True, help="Path to orca-slice-engine executable")
    parser.add_argument("--model", required=True, help="Path to test 3MF model file")
    parser.add_argument("--resources", help="Path to resources directory")
    parser.add_argument("--output", default="benchmark_results.json", help="Output JSON file")
    
    args = parser.parse_args()
    
    benchmark = CloudEngineBenchmark(
        engine_path=args.engine,
        model_path=args.model,
        resources_path=args.resources
    )
    
    suite = benchmark.run_all_tests()
    
    # Save JSON report
    with open(args.output, "w") as f:
        json.dump(asdict(suite), f, indent=2)
    
    print(f"\nDetailed report saved to: {args.output}")
    
    # Exit with appropriate code
    sys.exit(0 if suite.summary["failed"] == 0 else 1)


if __name__ == "__main__":
    main()
