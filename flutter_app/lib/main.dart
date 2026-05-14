import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

// ===================================================================
// Entrypoints: C++ creates each view with a specific entrypoint name.
//   homeMain    → HomePanelApp  (snapmaker/home)
//   prepareMain → PreparePanelApp (snapmaker/prepare)
// ===================================================================

@pragma('vm:entry-point')
void main() => runApp(const HomePanelApp());

@pragma('vm:entry-point')
void homeMain() => runApp(const HomePanelApp());

@pragma('vm:entry-point')
void prepareMain() => runApp(const PreparePanelApp());

// ============================ Shared Panel Widget ===========================

class PanelApp extends StatelessWidget {
  final String title;
  final Color color;
  final String channelName;
  const PanelApp(this.title, this.color, this.channelName, {super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: ThemeData(colorSchemeSeed: color, useMaterial3: true),
      home: PanelPage(title, color, channelName),
    );
  }
}

class PanelPage extends StatefulWidget {
  final String title, channelName;
  final Color color;
  const PanelPage(this.title, this.color, this.channelName, {super.key});

  @override
  State<PanelPage> createState() => _PanelPageState();
}

class _PanelPageState extends State<PanelPage> {
  late final MethodChannel _channel;
  int _counter = 0;
  final List<String> _messages = [];
  final TextEditingController _sendCtrl = TextEditingController();
  String _cxxVersion = '';

  @override
  void initState() {
    super.initState();
    _channel = MethodChannel(widget.channelName);
    _channel.setMethodCallHandler(_onMethodCall);
    _requestVersion();
  }

  Future<void> _requestVersion() async {
    try {
      final v = await _channel.invokeMethod<String>('getVersion');
      if (v != null && mounted) setState(() => _cxxVersion = v);
    } catch (_) {
      if (mounted) setState(() => _cxxVersion = '(not connected)');
    }
  }

  Future<dynamic> _onMethodCall(MethodCall call) async {
    switch (call.method) {
      case 'onMessage':
        final msg = call.arguments as String;
        if (mounted) {
          setState(() {
            _messages.insert(0, '[C++] $msg');
            if (_messages.length > 50) _messages.removeLast();
          });
        }
        return null;
      case 'onCounterUpdate':
        if (mounted) setState(() => _counter = call.arguments as int);
        return null;
      default:
        throw MissingPluginException();
    }
  }

  void _askCpp() async {
    try {
      final val = await _channel.invokeMethod<int>('incrementCounter', _counter);
      if (mounted && val != null) setState(() => _counter = val);
    } catch (_) {}
  }

  void _sendToCpp() async {
    final text = _sendCtrl.text.trim();
    if (text.isEmpty) return;
    try {
      final reply = await _channel.invokeMethod<String>('sendMessage', text);
      if (mounted && reply != null) {
        setState(() {
          _messages.insert(0, '[C++ reply] $reply');
          if (_messages.length > 50) _messages.removeLast();
        });
      }
    } catch (e) {
      if (mounted) {
        setState(() {
          _messages.insert(0, '[err] ${e.toString().substring(0, 60)}');
        });
      }
    }
    _sendCtrl.clear();
  }

  @override
  Widget build(BuildContext context) {
    final isDark = Theme.of(context).brightness == Brightness.dark;
    return Scaffold(
      backgroundColor: isDark ? const Color(0xFF1A1A2E) : const Color(0xFFF0F0F5),
      body: Column(
        children: [
          // Header
          Container(
            width: double.infinity,
            padding: const EdgeInsets.all(12),
            color: widget.color.withValues(alpha: 0.15),
            child: Row(
              children: [
                Icon(Icons.circle, size: 10, color: widget.color),
                const SizedBox(width: 8),
                Text(widget.title,
                    style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold,
                        color: widget.color)),
                const Spacer(),
                if (_cxxVersion.isNotEmpty)
                  Chip(label: Text('C++ $_cxxVersion', style: const TextStyle(fontSize: 10))),
              ],
            ),
          ),
          // Counter
          Padding(
            padding: const EdgeInsets.all(12),
            child: Row(mainAxisAlignment: MainAxisAlignment.center, children: [
              IconButton.filled(
                  onPressed: () => setState(() => _counter--),
                  icon: const Icon(Icons.remove)),
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 16),
                child: Text('$_counter',
                    style: const TextStyle(fontSize: 32, fontWeight: FontWeight.bold)),
              ),
              IconButton.filled(
                  onPressed: () => setState(() => _counter++),
                  icon: const Icon(Icons.add)),
              const SizedBox(width: 12),
              FilledButton.tonal(onPressed: _askCpp, child: const Text('Ask C++')),
            ]),
          ),
          const Divider(height: 1),
          // Messages
          Expanded(
            child: _messages.isEmpty
                ? const Center(
                    child: Text('No messages yet',
                        style: TextStyle(color: Colors.grey, fontSize: 12)))
                : ListView.builder(
                    itemCount: _messages.length,
                    itemBuilder: (_, i) => ListTile(
                      dense: true,
                      leading: Icon(Icons.message, size: 14, color: widget.color),
                      title: Text(_messages[i], style: const TextStyle(fontSize: 11)),
                    ),
                  ),
          ),
          const Divider(height: 1),
          // Input
          Padding(
            padding: const EdgeInsets.all(8),
            child: Row(children: [
              Expanded(
                child: TextField(
                  controller: _sendCtrl,
                  style: const TextStyle(fontSize: 13),
                  decoration: const InputDecoration(
                    hintText: 'Send to C++...',
                    border: OutlineInputBorder(),
                    isDense: true,
                    contentPadding:
                        EdgeInsets.symmetric(horizontal: 10, vertical: 8),
                  ),
                  onSubmitted: (_) => _sendToCpp(),
                ),
              ),
              const SizedBox(width: 6),
              FilledButton(onPressed: _sendToCpp, child: const Text('Send')),
            ]),
          ),
        ],
      ),
    );
  }
}

// ============================ Entrypoint Wrappers ===========================

class HomePanelApp extends StatelessWidget {
  const HomePanelApp({super.key});
  @override
  Widget build(BuildContext context) =>
      const PanelApp('Home Panel', Colors.blue, 'snapmaker/home');
}

class PreparePanelApp extends StatelessWidget {
  const PreparePanelApp({super.key});
  @override
  Widget build(BuildContext context) =>
      const PanelApp('Prepare Panel', Colors.teal, 'snapmaker/prepare');
}
