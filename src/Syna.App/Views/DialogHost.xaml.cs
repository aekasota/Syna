using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;

namespace Syna.Views;

/// <summary>Modal confirm/prompt dialogs in the product's own style.</summary>
public sealed partial class DialogHost : UserControl
{
    private TaskCompletionSource<string?>? _pending;

    public DialogHost() => InitializeComponent();

    public bool IsOpen => _pending != null;

    public async Task<bool> ConfirmAsync(string title, string message, string confirm) =>
        await ShowAsync(title, message, confirm, input: null) != null;

    public Task<string?> PromptAsync(string title, string initial, string confirm) => ShowAsync(title, "", confirm, initial);

    private Task<string?> ShowAsync(string title, string message, string confirm, string? input)
    {
        _pending?.TrySetResult(null);
        _pending = new TaskCompletionSource<string?>();
        TitleText.Text = title;
        MessageText.Text = message;
        MessageText.Visibility = message.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        ConfirmButton.Content = confirm;
        Input.Visibility = input != null ? Visibility.Visible : Visibility.Collapsed;
        Input.Text = input ?? "";
        Scrim.Visibility = Visibility.Visible;
        Scrim.Opacity = 1;
        if (input != null)
        {
            Input.Focus(FocusState.Programmatic);
            int dot = input.LastIndexOf('.');
            Input.Select(0, dot > 0 ? dot : input.Length);
        }
        else
        {
            CancelButton.Focus(FocusState.Programmatic);
        }
        return _pending.Task;
    }

    private void Close(string? result)
    {
        var pending = _pending;
        _pending = null;
        Scrim.Opacity = 0;
        Scrim.Visibility = Visibility.Collapsed;
        pending?.TrySetResult(result);
    }

    private void OnConfirm(object sender, RoutedEventArgs e) => Close(Input.Visibility == Visibility.Visible ? Input.Text : "ok");
    private void OnCancel(object sender, RoutedEventArgs e) => Close(null);

    private void OnKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Escape)
        {
            Close(null);
            e.Handled = true;
        }
        else if (e.Key == VirtualKey.Enter && (Input.Visibility == Visibility.Visible || !ReferenceEquals(FocusManager.GetFocusedElement(XamlRoot), CancelButton)))
        {
            OnConfirm(this, new RoutedEventArgs());
            e.Handled = true;
        }
    }
}
